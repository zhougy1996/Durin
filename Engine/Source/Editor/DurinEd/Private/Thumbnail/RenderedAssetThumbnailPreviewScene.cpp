#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

#include "Asset/EditorAssetRetention.h"
#include "Components/DirectionalLightComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IRendererModule.h"
#include "IScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr FPrimitiveSceneId ThumbnailPrimitiveId = 1;
		constexpr double RotationTolerance = 1.0e-8;

		auto RotationFromForward(const FVector3& Direction) -> FQuat
		{
			const FVector3 To = glm::normalize(Direction);
			const double Dot = glm::dot(FVectorConstants::Forward, To);
			if (Dot > 1.0 - RotationTolerance) return glm::identity<FQuat>();
			if (Dot < -1.0 + RotationTolerance)
				return glm::angleAxis(glm::pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = glm::cross(FVectorConstants::Forward, To);
			return glm::normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}

		auto BuildThumbnailView(const FRenderedAssetThumbnailVisualContract& Contract) -> FSceneView
		{
			FSceneView View;
			const FVector3 CameraDirection(
				Contract.CameraDirectionX, Contract.CameraDirectionY, Contract.CameraDirectionZ);
			const FVector3 Eye =
				glm::normalize(CameraDirection) * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = glm::normalize(-Eye);
			const FVector3 Right = glm::normalize(glm::cross(FVectorConstants::Up, Forward));
			const FVector3 Up = glm::normalize(glm::cross(Forward, Right));
			View.ViewLocation = Eye;
			View.ViewMatrix[0][0] = Forward.x;
			View.ViewMatrix[1][0] = Forward.y;
			View.ViewMatrix[2][0] = Forward.z;
			View.ViewMatrix[3][0] = -glm::dot(Forward, Eye);
			View.ViewMatrix[0][1] = Right.x;
			View.ViewMatrix[1][1] = Right.y;
			View.ViewMatrix[2][1] = Right.z;
			View.ViewMatrix[3][1] = -glm::dot(Right, Eye);
			View.ViewMatrix[0][2] = Up.x;
			View.ViewMatrix[1][2] = Up.y;
			View.ViewMatrix[2][2] = Up.z;
			View.ViewMatrix[3][2] = -glm::dot(Up, Eye);

			const float AspectRatio = static_cast<float>(Contract.Output.Width)
				/ static_cast<float>(Contract.Output.Height);
			const float YScale =
				1.0f / std::tan(glm::radians(Contract.VerticalFieldOfViewDegrees) * 0.5f);
			const float XScale = YScale / std::max(AspectRatio, 0.001f);
			const float DepthScale =
				Contract.FarClipDistance / (Contract.FarClipDistance - Contract.NearClipDistance);
			const float DepthBias =
				-Contract.NearClipDistance * Contract.FarClipDistance
				/ (Contract.FarClipDistance - Contract.NearClipDistance);
			View.ProjectionMatrix = FMatrix(0.0f);
			View.ProjectionMatrix[1][0] = XScale;
			View.ProjectionMatrix[2][1] = -YScale;
			View.ProjectionMatrix[0][2] = DepthScale;
			View.ProjectionMatrix[3][2] = DepthBias;
			View.ProjectionMatrix[0][3] = 1.0f;
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Contract.Output.Width;
			View.ViewportHeight = Contract.Output.Height;
			View.ClearColor = {
				Contract.BackgroundRed,
				Contract.BackgroundGreen,
				Contract.BackgroundBlue,
				Contract.bOutputOpaque ? 1.0f : Contract.BackgroundAlpha};
			return View;
		}
	}

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
		std::unique_ptr<IScene> Scene;
		FRetainedEditorAsset SphereAsset;
		TObjectPtr<DDirectionalLightComponent> Light;
		FTextureRHIRef RenderTarget;
		FSceneView View;
		std::shared_ptr<FCapture> Capture = std::make_shared<FCapture>();
		std::string Error;
		bool bHasPrimitive = false;

		FImpl(FRenderedAssetThumbnailVisualContract InContract, const FAssetThumbnailBudgets& Budgets)
			: Contract(std::move(InContract))
		{
			checkf(IsInGameThread(), "Rendered thumbnail preview scenes must be created on the game thread.");
			if (Budgets.MaximumLivePreviewScenes == 0)
			{
				Error = "The rendered-thumbnail live-scene budget is zero.";
				return;
			}
			if (Contract.Output.Width == 0 || Contract.Output.Height == 0)
			{
				Error = "The rendered-thumbnail output dimensions are invalid.";
				return;
			}
			if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr || GDynamicRHI == nullptr)
			{
				Error = "The renderer is not available.";
				return;
			}
			Scene = GEngine->GetRendererModule()->CreateScene();
			FAssetPath SpherePath;
			if (!FAssetPath::TryCreate(
					FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath, &Error)
				|| !FEditorAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)
				|| Cast<DStaticMesh>(SphereAsset.Get()) == nullptr)
			{
				if (Error.empty()) Error = "The shared preview sphere is not a StaticMesh.";
				return;
			}

			Light = NewObject<DDirectionalLightComponent>(
				GEngine, "RenderedAssetThumbnailPreviewLight");
			AddToRoot(Light.Get());
			Light->SetWorldRotation(RotationFromForward({
				Contract.KeyLightDirectionX,
				Contract.KeyLightDirectionY,
				Contract.KeyLightDirectionZ}));
			Light->SetIntensity(Contract.KeyLightIntensity);
			Light->SetAmbientIntensity(Contract.FillLightIntensity);
			Light->SetRimLightIntensity(0.16f);
			Scene->AddDirectionalLight(Light);
			View = BuildThumbnailView(Contract);

			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"RenderedAssetThumbnailOutput",
				Contract.Output.Width,
				Contract.Output.Height,
				EPixelFormat::SRGBA8_UNORM);
			Desc.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::CPUReadback);
			RenderTarget = RHICreateTexture(Desc);
			if (RenderTarget == nullptr) Error = "Failed to create the rendered-thumbnail output target.";
		}

		~FImpl()
		{
			checkf(IsInGameThread(), "Rendered thumbnail preview scenes must be destroyed on the game thread.");
			{
				std::lock_guard Lock(Capture->Mutex);
				++Capture->Generation;
			}
			if (GRenderingThread) FlushRenderingCommands();
			RenderTarget = nullptr;
			if (Scene != nullptr)
			{
				Scene->Release();
				if (GRenderingThread) FlushRenderingCommands();
				Scene.reset();
			}
			RemoveFromRoot(Light.Get());
		}
	};

	FRenderedAssetThumbnailPreviewScenePool::FRenderedAssetThumbnailPreviewScenePool(
		FRenderedAssetThumbnailVisualContract VisualContract,
		FAssetThumbnailBudgets Budgets)
		: Impl(std::make_unique<FImpl>(std::move(VisualContract), Budgets))
	{
	}

	FRenderedAssetThumbnailPreviewScenePool::~FRenderedAssetThumbnailPreviewScenePool() = default;

	auto FRenderedAssetThumbnailPreviewScenePool::IsAvailable() const -> bool
	{
		return Impl->Error.empty() && Impl->Scene != nullptr && Impl->RenderTarget != nullptr;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::GetDiagnostic() const -> std::string
	{
		return Impl->Error;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::GetSphereMesh() const -> DStaticMesh*
	{
		return Cast<DStaticMesh>(Impl->SphereAsset.Get());
	}

	auto FRenderedAssetThumbnailPreviewScenePool::SetPrimitive(
		std::unique_ptr<PrimitiveSceneProxy> Proxy,
		const FMatrix& Transform,
		std::string& OutError) -> bool
	{
		checkf(IsInGameThread(), "Rendered thumbnail scene mutation must run on the game thread.");
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
		if (Proxy == nullptr)
		{
			OutError = "The rendered-thumbnail primitive is null.";
			return false;
		}
		Impl->Scene->AddOrReplacePrimitive(ThumbnailPrimitiveId, std::move(Proxy), Transform);
		Impl->bHasPrimitive = true;
		return true;
	}

	auto FRenderedAssetThumbnailPreviewScenePool::BeginCapture(
		std::string& OutError,
		bool bOutputOpaque) -> bool
	{
		checkf(IsInGameThread(), "Rendered thumbnail capture must start on the game thread.");
		OutError.clear();
		if (!IsAvailable() || !Impl->bHasPrimitive)
		{
			OutError = IsAvailable()
				? "The rendered-thumbnail preview scene has no primitive."
				: Impl->Error;
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
		IScene* Scene = Impl->Scene.get();
		FTextureRHIRef RenderTarget = Impl->RenderTarget;
		FSceneView View = Impl->View;
		if (!bOutputOpaque) View.ClearColor = FVector4f(0.0f);
		ENQUEUE_RENDER_COMMAND(RenderAssetThumbnailPreview)(
			[Capture, Generation, Renderer, Scene, RenderTarget, View](
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
					Renderer->PrepareSceneResources(CommandList, Scene);
					Renderer->RenderView(CommandList, Scene, View, RenderTarget, false);
					if (!GDynamicRHI->RHIReadTexture2D(
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
		checkf(IsInGameThread(), "Rendered thumbnail capture polling must run on the game thread.");
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
		checkf(IsInGameThread(), "Rendered thumbnail scene reset must run on the game thread.");
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			++Impl->Capture->Generation;
			Impl->Capture->State = ERenderedAssetThumbnailCaptureState::Idle;
			Impl->Capture->Pixels.clear();
			Impl->Capture->Error.clear();
		}
		if (Impl->Scene != nullptr && Impl->bHasPrimitive)
			Impl->Scene->RemovePrimitive(ThumbnailPrimitiveId);
		Impl->bHasPrimitive = false;
	}
} // namespace Durin
