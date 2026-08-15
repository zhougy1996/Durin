#include "Thumbnail/RenderedAssetThumbnailPreviewScene.h"

#include "Components/DirectionalLightComponent.h"
#include "DynamicRHI.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "IScene.h"
#include "Math/Operations.h"
#include "Preview/PreviewScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneViewProjection.h"

namespace Durin::Editor
{
	namespace
	{
		constexpr double RotationTolerance = 1.0e-8;

		auto RotationFromForward(const FVector3& Direction) -> FQuat
		{
			const FVector3 To = Math::Normalize(Direction);
			const double Dot = Math::Dot(FVectorConstants::Forward, To);
			if (Dot > 1.0 - RotationTolerance) return FQuatConstants::Identity;
			if (Dot < -1.0 + RotationTolerance)
				return Math::MakeQuaternionFromAxisAngleRadians(
					Math::Pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = Math::Cross(FVectorConstants::Forward, To);
			return Math::Normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}

		auto BuildView(
			const FAssetThumbnailOutputSettings& Output,
			const FRenderedAssetThumbnailPreviewView& Preview) -> FSceneView
		{
			FSceneView View;
			const FVector3 Eye(
				Preview.CameraPosition[0],
				Preview.CameraPosition[1],
				Preview.CameraPosition[2]);
			const FVector3 Forward(
				Preview.CameraForward[0],
				Preview.CameraForward[1],
				Preview.CameraForward[2]);
			const FVector3 Right(
				Preview.CameraRight[0],
				Preview.CameraRight[1],
				Preview.CameraRight[2]);
			const FVector3 Up(
				Preview.CameraUp[0],
				Preview.CameraUp[1],
				Preview.CameraUp[2]);
			View.ViewLocation = Eye;
			View.ViewMatrix[0][0] = Forward.x;
			View.ViewMatrix[1][0] = Forward.y;
			View.ViewMatrix[2][0] = Forward.z;
			View.ViewMatrix[3][0] = -Math::Dot(Forward, Eye);
			View.ViewMatrix[0][1] = Right.x;
			View.ViewMatrix[1][1] = Right.y;
			View.ViewMatrix[2][1] = Right.z;
			View.ViewMatrix[3][1] = -Math::Dot(Right, Eye);
			View.ViewMatrix[0][2] = Up.x;
			View.ViewMatrix[1][2] = Up.y;
			View.ViewMatrix[2][2] = Up.z;
			View.ViewMatrix[3][2] = -Math::Dot(Up, Eye);

			const float AspectRatio = static_cast<float>(Output.Width)
				/ static_cast<float>(Output.Height);
			const float NearClip = static_cast<float>(Preview.NearClipDistance);
			const float FarClip = static_cast<float>(Preview.FarClipDistance);
			const bool bValidProjection = SceneViewProjection::BuildPerspectiveProjection(
				Preview.VerticalFieldOfViewDegrees, AspectRatio, NearClip, FarClip,
				ESceneDepthConvention::ReversedZ, View.ProjectionMatrix);
			check(bValidProjection);
			View.NearClipDistance = NearClip;
			View.FarClipDistance = FarClip;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Output.Width;
			View.ViewportHeight = Output.Height;
			View.Settings.LODMode = Preview.bForceLOD0
				? EViewLODMode::ForceLOD0 : EViewLODMode::Automatic;
			View.ClearColor = {
				Preview.ClearRed,
				Preview.ClearGreen,
				Preview.ClearBlue,
				Preview.ClearAlpha};
			return View;
		}

		auto MakeDefaultPreviewView() -> FRenderedAssetThumbnailPreviewView
		{
			const FRenderedAssetThumbnailVisualContract Contract;
			const FVector3 Eye = Math::Normalize(FVector3(
				Contract.CameraDirectionX,
				Contract.CameraDirectionY,
				Contract.CameraDirectionZ)) * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = Math::Normalize(-Eye);
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			return {
				.CameraPosition = {Eye.x, Eye.y, Eye.z},
				.CameraForward = {Forward.x, Forward.y, Forward.z},
				.CameraRight = {Right.x, Right.y, Right.z},
				.CameraUp = {Up.x, Up.y, Up.z},
				.VerticalFieldOfViewDegrees = Contract.VerticalFieldOfViewDegrees,
				.NearClipDistance = Contract.NearClipDistance,
				.FarClipDistance = Contract.FarClipDistance,
				.ClearRed = Contract.BackgroundRed,
				.ClearGreen = Contract.BackgroundGreen,
				.ClearBlue = Contract.BackgroundBlue,
				.ClearAlpha = Contract.bOutputOpaque ? 1.0f : Contract.BackgroundAlpha};
		}

		auto GetRenderFailureDiagnostic(ERenderViewResult Result) -> std::string
		{
			switch (Result)
			{
			case ERenderViewResult::Success:
				return {};
			case ERenderViewResult::InvalidOutput:
				return "The rendered-thumbnail output target is invalid.";
			case ERenderViewResult::RendererResourcesUnavailable:
				return "The rendered-thumbnail renderer resources are unavailable.";
			case ERenderViewResult::RequiredEnvironmentUnavailable:
				return "The rendered-thumbnail view environment is unavailable.";
			}
			return "The rendered-thumbnail renderer returned an unknown failure.";
		}
	} // namespace

	struct FRenderedAssetThumbnailPreviewScenePool::FImpl
	{
		struct FCapture
		{
			std::mutex Mutex;
			ERenderedAssetThumbnailCaptureState State =
				ERenderedAssetThumbnailCaptureState::Idle;
			std::vector<uint8> Pixels;
			std::string Error;
			uint64 Generation = 0;
		};

		FRenderedAssetThumbnailVisualContract Contract;
		FAssetThumbnailOutputSettings Output;
		std::unique_ptr<Editor::FPreviewScene> PreviewScene;
		TObjectPtr<DDirectionalLightComponent> Light;
		FTextureRHIRef RenderTarget;
		FSceneView View;
		std::optional<FViewEnvironmentOverride> Environment;
		std::shared_ptr<FCapture> Capture = std::make_shared<FCapture>();
		std::string Error;

		FImpl(
			FRenderedAssetThumbnailVisualContract InContract,
			const FAssetThumbnailBudgets& Budgets)
			: Contract(std::move(InContract))
			, Output(Contract.Output)
		{
			checkf(IsInGameThread(),
				"Rendered thumbnail preview scenes must be created on the game thread.");
			if (Budgets.MaximumLivePreviewScenes == 0)
			{
				Error = "The rendered-thumbnail live-scene budget is zero.";
				return;
			}
			if (Output.Width == 0 || Output.Height == 0)
			{
				Error = "The rendered-thumbnail output dimensions are invalid.";
				return;
			}
			if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr
				|| GDynamicRHI == nullptr)
			{
				Error = "The renderer is not available.";
				return;
			}
			PreviewScene = std::make_unique<Editor::FPreviewScene>("RenderedAssetThumbnailPreview");
			if (!PreviewScene->IsAvailable())
			{
				Error = PreviewScene->GetDiagnostic();
				return;
			}
			AActor* LightActor = PreviewScene->GetWorld()->SpawnActor<AActor>(
				"RenderedAssetThumbnailLightActor");
			Light = LightActor
				? Cast<DDirectionalLightComponent>(LightActor->AddInstanceComponent(
					DDirectionalLightComponent::StaticClass(), "PreviewLight"))
				: nullptr;
			if (Light == nullptr)
			{
				Error = "The rendered-thumbnail preview light could not be created.";
				return;
			}
			Light->SetWorldRotation(RotationFromForward({
				Contract.KeyLightDirectionX,
				Contract.KeyLightDirectionY,
				Contract.KeyLightDirectionZ}));
			Light->SetIntensity(Contract.KeyLightIntensity);
			Light->SetAmbientIntensity(Contract.FillLightIntensity);
			Light->SetRimLightIntensity(0.16f);
			View = BuildView(Output, MakeDefaultPreviewView());

			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"RenderedAssetThumbnailOutput",
				Output.Width,
				Output.Height,
				EPixelFormat::SRGBA8_UNORM);
			Desc.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::CPUReadback);
			RenderTarget = RHICreateTexture(Desc);
			if (RenderTarget == nullptr)
				Error = "Failed to create the rendered-thumbnail output target.";
		}

		~FImpl()
		{
			checkf(IsInGameThread(),
				"Rendered thumbnail preview scenes must be destroyed on the game thread.");
			{
				std::lock_guard Lock(Capture->Mutex);
				++Capture->Generation;
			}
			if (GRenderingThread) FlushRenderingCommands();
			RenderTarget = nullptr;
			PreviewScene.reset();
		}
	};

	FRenderedAssetThumbnailPreviewScenePool::FRenderedAssetThumbnailPreviewScenePool(
		FAssetThumbnailOutputSettings Output,
		FAssetThumbnailBudgets Budgets)
		: Impl([&Output, &Budgets] {
			FRenderedAssetThumbnailVisualContract Contract;
			Contract.Output = std::move(Output);
			return std::make_unique<FImpl>(std::move(Contract), Budgets);
		}())
	{
	}

	FRenderedAssetThumbnailPreviewScenePool::FRenderedAssetThumbnailPreviewScenePool(
		FRenderedAssetThumbnailVisualContract VisualContract,
		FAssetThumbnailBudgets Budgets)
		: Impl(std::make_unique<FImpl>(std::move(VisualContract), Budgets))
	{
	}

	FRenderedAssetThumbnailPreviewScenePool::~FRenderedAssetThumbnailPreviewScenePool() = default;

	auto FRenderedAssetThumbnailPreviewScenePool::IsAvailable() const -> bool
	{
		return Impl->Error.empty() && Impl->PreviewScene != nullptr
			&& Impl->PreviewScene->IsAvailable() && Impl->RenderTarget != nullptr;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::GetDiagnostic() const -> std::string
	{
		return Impl->Error;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::GetWorld() -> DWorld*
	{
		return IsAvailable() ? Impl->PreviewScene->GetWorld() : nullptr;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::SetView(
		const FRenderedAssetThumbnailPreviewView& View,
		std::string& OutError) -> bool
	{
		checkf(IsInGameThread(),
			"Rendered thumbnail scene mutation must run on the game thread.");
		OutError.clear();
		if (!IsAvailable())
		{
			OutError = Impl->Error;
			return false;
		}
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			if (Impl->Capture->State == ERenderedAssetThumbnailCaptureState::Rendering)
			{
				OutError = "A rendered-thumbnail capture is already in flight.";
				return false;
			}
		}
		if (View.NearClipDistance <= 0.0
			|| View.FarClipDistance <= View.NearClipDistance
			|| View.VerticalFieldOfViewDegrees <= 0.0
			|| View.VerticalFieldOfViewDegrees >= 180.0)
		{
			OutError = "The rendered-thumbnail preview view is invalid.";
			return false;
		}
		Impl->View = BuildView(Impl->Output, View);
		return true;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::SetViewEnvironment(
		const FViewEnvironmentOverride& Environment,
		std::string& OutError) -> bool
	{
		checkf(IsInGameThread(),
			"Rendered thumbnail scene mutation must run on the game thread.");
		OutError.clear();
		if (!IsAvailable())
		{
			OutError = Impl->Error;
			return false;
		}
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			if (Impl->Capture->State == ERenderedAssetThumbnailCaptureState::Rendering)
			{
				OutError = "A rendered-thumbnail capture is already in flight.";
				return false;
			}
		}
		if (Environment.TextureReference == nullptr
			|| !Math::IsFinite(Environment.Rotation)
			|| Math::LengthSquared(Environment.Rotation) <= kDoubleSmallNumber
			|| !Math::IsFinite(Environment.Tint)
			|| !std::isfinite(Environment.Intensity)
			|| Environment.Intensity < 0.0f)
		{
			OutError = "The rendered-thumbnail view environment is invalid.";
			return false;
		}
		Impl->Environment = Environment;
		Impl->Environment->Rotation = Math::Normalize(Environment.Rotation);
		return true;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::BeginCapture(
		std::string& OutError) -> bool
	{
		checkf(IsInGameThread(),
			"Rendered thumbnail capture must start on the game thread.");
		OutError.clear();
		if (!IsAvailable())
		{
			OutError = Impl->Error;
			return false;
		}

		uint64 Generation = 0;
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			if (Impl->Capture->State == ERenderedAssetThumbnailCaptureState::Rendering)
			{
				OutError = "A rendered-thumbnail capture is already in flight.";
				return false;
			}
			Impl->Capture->State = ERenderedAssetThumbnailCaptureState::Rendering;
			Impl->Capture->Pixels.clear();
			Impl->Capture->Error.clear();
			Generation = ++Impl->Capture->Generation;
		}

		std::shared_ptr Capture = Impl->Capture;
		IRendererModule* Renderer = GEngine->GetRendererModule();
		IScene* Scene = Impl->PreviewScene->GetRenderScene();
		FTextureRHIRef RenderTarget = Impl->RenderTarget;
		const FSceneView View = Impl->View;
		const FSceneViewRenderOptions Options{
			.Environment = Impl->Environment,
			.HybridOpaqueRoute = EHybridOpaqueRoute::DeferredRequired};
		ENQUEUE_RENDER_COMMAND(RenderAssetThumbnailPreview)(
			[Capture, Generation, Renderer, Scene, RenderTarget, View, Options](
				FRHICommandListImmediate& CommandList) {
				std::vector<uint8> Pixels;
				std::string Error;
				if (Renderer == nullptr || Scene == nullptr || RenderTarget == nullptr)
				{
					Error = "Rendered-thumbnail resources were released before capture.";
				}
				else
				{
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					const ERenderViewResult RenderResult = Renderer->RenderView(
						CommandList, Scene, View, RenderTarget, false, Options);
					if (RenderResult != ERenderViewResult::Success)
						Error = GetRenderFailureDiagnostic(RenderResult);
					else if (!GDynamicRHI->RHIReadTexture2D(
							CommandList, RenderTarget, 0, 0, Pixels))
						Error = "Failed to read back the rendered-thumbnail output.";
				}
				std::lock_guard Lock(Capture->Mutex);
				if (Capture->Generation != Generation) return;
				Capture->Pixels = std::move(Pixels);
				Capture->Error = std::move(Error);
				Capture->State = Capture->Error.empty()
					? ERenderedAssetThumbnailCaptureState::Ready
					: ERenderedAssetThumbnailCaptureState::Failed;
			});
		return true;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::PollCapture(
		std::vector<uint8>& OutPixels,
		std::string& OutError) -> ERenderedAssetThumbnailCaptureState
	{
		checkf(IsInGameThread(),
			"Rendered thumbnail capture polling must run on the game thread.");
		OutPixels.clear();
		OutError.clear();
		std::lock_guard Lock(Impl->Capture->Mutex);
		if (Impl->Capture->State == ERenderedAssetThumbnailCaptureState::Ready)
			OutPixels = std::move(Impl->Capture->Pixels);
		else if (Impl->Capture->State == ERenderedAssetThumbnailCaptureState::Failed)
			OutError = Impl->Capture->Error;
		return Impl->Capture->State;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::Reset() -> void
	{
		checkf(IsInGameThread(),
			"Rendered thumbnail scene reset must run on the game thread.");
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			++Impl->Capture->Generation;
			Impl->Capture->State = ERenderedAssetThumbnailCaptureState::Idle;
			Impl->Capture->Pixels.clear();
			Impl->Capture->Error.clear();
		}
		Impl->View = BuildView(Impl->Output, MakeDefaultPreviewView());
		Impl->Environment.reset();
	}
} // namespace Durin::Editor
