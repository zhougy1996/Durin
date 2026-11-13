#include "Thumbnail/ThumbnailCapture.h"
#include "Thumbnail/ThumbnailPreviewScene.h"

#include "Components/PrimitiveComponent.h"
#include "DynamicRHI.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"
#include "Math/Operations.h"
#include "PSOPrecacheComponent.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"
#include "RenderPipelineCreation.h"
#include "SceneViewProjection.h"

namespace Durin
{
	namespace
	{
		auto BuildView(
			const FAssetThumbnailOutputSettings& Output,
			const FThumbnailPreviewView& Preview
		) -> FSceneView
		{
			FSceneView View;
			View.MaterialTimeSeconds = 0.0;
			const FVector3 Eye(
				Preview.CameraPosition[0],
				Preview.CameraPosition[1],
				Preview.CameraPosition[2]
			);
			const FVector3 Forward(
				Preview.CameraForward[0],
				Preview.CameraForward[1],
				Preview.CameraForward[2]
			);
			const FVector3 Right(
				Preview.CameraRight[0],
				Preview.CameraRight[1],
				Preview.CameraRight[2]
			);
			const FVector3 Up(
				Preview.CameraUp[0],
				Preview.CameraUp[1],
				Preview.CameraUp[2]
			);
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
				ESceneDepthConvention::ReversedZ, View.ProjectionMatrix
			);
			check(bValidProjection);
			View.NearClipDistance = NearClip;
			View.FarClipDistance = FarClip;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Output.Width;
			View.ViewportHeight = Output.Height;
			View.Settings.Mode.LODMode = Preview.bForceLOD0 ? EViewLODMode::ForceLOD0 : EViewLODMode::Automatic;
			View.ClearColor = {
				Preview.ClearRed,
				Preview.ClearGreen,
				Preview.ClearBlue,
				Preview.ClearAlpha
			};
			return View;
		}

		auto MakeDefaultPreviewView() -> FThumbnailPreviewView
		{
			const FThumbnailVisualContract Contract;
			const FVector3 Eye = Math::Normalize(FVector3(
									 Contract.CameraDirectionX,
									 Contract.CameraDirectionY,
									 Contract.CameraDirectionZ
								 ))
								 * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = Math::Normalize(-Eye);
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward)
			);
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
				.ClearAlpha = Contract.bOutputOpaque ? 1.0f : Contract.BackgroundAlpha
			};
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
			case ERenderViewResult::RendererResourcesPending:
				return "The rendered-thumbnail renderer resources are unavailable.";
			case ERenderViewResult::RequiredEnvironmentUnavailable:
				return "The rendered-thumbnail view environment is unavailable.";
			}
			return "The rendered-thumbnail renderer returned an unknown failure.";
		}

		auto HasRejectedVisibleGeometry(const FSceneViewStatistics& Statistics)
			-> bool
		{
			if (Statistics.Visibility.VisiblePrimitives == 0) return false;
			return Statistics.StaticMesh.DrawCalls == 0;
		}
	} // namespace

	struct FThumbnailCapture::FImpl
	{
		struct FCapture
		{
			std::mutex Mutex;
			EThumbnailCaptureState State =
				EThumbnailCaptureState::Idle;
			FByteBuffer Pixels;
			std::shared_ptr<FRHITextureReadback> Readback;
			FShaderMapPreparation Shaders;
			FRenderResourcePreparation Preparation;
			std::string Error;
			uint64 Generation = 0;
		};

		FAssetThumbnailOutputSettings Output;
		FThumbnailPreviewScene* Scene = nullptr;
		FTextureRHIRef RenderTarget;
		FThumbnailPreviewView View;
		std::optional<FViewEnvironmentOverride> Environment;
		FThumbnailImageRenderer ImageRenderer;
		std::shared_ptr<FCapture> Capture = std::make_shared<FCapture>();
		std::string Error;

		auto CheckCanConfigure() -> void
		{
			checkf(IsInGameThread(), "Thumbnail capture setup belongs to the game thread.");
			std::lock_guard Lock(Capture->Mutex);
			checkf(Capture->State != EThumbnailCaptureState::Preparing
				&& Capture->State != EThumbnailCaptureState::Rendering
				&& Capture->State != EThumbnailCaptureState::ReadbackPending,
				"Reset thumbnail capture before changing its setup.");
		}

		explicit FImpl(FAssetThumbnailOutputSettings InOutput) : Output(InOutput)
		{
			checkf(IsInGameThread(), "Thumbnail captures belong to the game thread.");
			if (Output.Width == 0 || Output.Height == 0 || !GDynamicRHI)
			{
				Error = "Thumbnail output dimensions or renderer are unavailable.";
				return;
			}
			View = MakeDefaultPreviewView();
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"RenderedAssetThumbnailOutput",
				Output.Width,
				Output.Height,
				EPixelFormat::SRGBA8_UNORM
			);
			Desc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
			RenderTarget = RHICreateTexture(Desc);
		}

	};

	FThumbnailCapture::FThumbnailCapture(FAssetThumbnailOutputSettings Output)
		: Impl(std::make_unique<FImpl>(Output)) {}
	FThumbnailCapture::~FThumbnailCapture()
	{
		Reset();
		if (GRenderingThread) FlushRenderingCommands();
	}
	auto FThumbnailCapture::IsAvailable() const -> bool
	{
		return Impl->Error.empty() && Impl->RenderTarget != nullptr;
	}
	auto FThumbnailCapture::GetDiagnostic() const -> std::string { return Impl->Error; }
	auto FThumbnailCapture::SetScene(FThumbnailPreviewScene& Scene) -> std::expected<void, std::string>
	{
		Impl->CheckCanConfigure();
		if (Impl->Scene == &Scene) return {};
		if (Impl->Scene || Scene.BindingOwner)
			return std::unexpected("The thumbnail preview scene is already in use.");
		if (!Scene.IsAvailable()) return std::unexpected(Scene.GetDiagnostic());
		Scene.BindingOwner = this;
		Impl->Scene = &Scene;
		return {};
	}

	auto FThumbnailCapture::BindDrawScene(FThumbnailPreviewScene& Scene) -> void
	{
		Impl->CheckCanConfigure();
		Impl->Scene = &Scene;
	}

	auto FThumbnailCapture::SetView(const FThumbnailPreviewView& View) -> void
	{
		Impl->CheckCanConfigure();
		Impl->View = View;
	}

	auto FThumbnailCapture::SetViewEnvironment(const FViewEnvironmentOverride& Environment) -> void
	{
		Impl->CheckCanConfigure();
		Impl->Environment = Environment;
	}

	auto FThumbnailCapture::SetImageRenderer(FThumbnailImageRenderer Renderer) -> void
	{
		Impl->CheckCanConfigure();
		Impl->ImageRenderer = std::move(Renderer);
	}

	auto FThumbnailCapture::BeginCapture() -> std::expected<void, std::string>
	{
		checkf(IsInGameThread(), "Rendered thumbnail capture must start on the game thread.");
		if (!IsAvailable())
		{
			return std::unexpected(Impl->Error);
		}

		if (!Impl->ImageRenderer && !Impl->Scene)
			return std::unexpected("The thumbnail capture has no preview scene or image renderer.");

		const auto& ViewSetup = Impl->View;
		if (ViewSetup.NearClipDistance <= 0.0 || ViewSetup.FarClipDistance <= ViewSetup.NearClipDistance
			|| ViewSetup.VerticalFieldOfViewDegrees <= 0.0 || ViewSetup.VerticalFieldOfViewDegrees >= 180.0)
			return std::unexpected("The rendered-thumbnail preview view is invalid.");
		if (const auto& Environment = Impl->Environment; Environment
			&& (!Environment->TextureReference || !Math::IsFinite(Environment->Rotation)
				|| Math::LengthSquared(Environment->Rotation) <= DoubleSmallNumber
				|| !Math::IsFinite(Environment->Tint) || !std::isfinite(Environment->Intensity)
				|| Environment->Intensity < 0.0f))
			return std::unexpected("The rendered-thumbnail view environment is invalid.");

		uint64 Generation = 0;
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			if (Impl->Capture->State == EThumbnailCaptureState::Rendering
				|| Impl->Capture->State == EThumbnailCaptureState::ReadbackPending)
			{
				return std::unexpected("A rendered-thumbnail capture is already in flight.");
			}
			if (Impl->Capture->State != EThumbnailCaptureState::Preparing)
				Impl->Capture->Preparation = FRenderResourcePreparation::Create();
			Impl->Capture->State = EThumbnailCaptureState::Rendering;
			Impl->Capture->Pixels.clear();
			Impl->Capture->Error.clear();
			Generation = ++Impl->Capture->Generation;
		}

		std::shared_ptr Capture = Impl->Capture;
		if (auto* World = Impl->Scene ? Impl->Scene->GetWorld() : nullptr)
		{
			DPrimitiveComponent::ProcessPSOPrecacheRequests(World);
			for (const auto& Actor : World->GetActors())
			{
				if (!Actor || Actor->IsHidden()) continue;
				for (const auto& Owned : Actor->GetComponents())
					if (auto* Primitive = Cast<DPrimitiveComponent>(Owned.Get()); Primitive && Primitive->IsVisible())
					{
						const bool bPending = Primitive->CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority::Highest);
						if (IsComponentPSOPrecachingEnabled()
							&& !Primitive->GetPSOPrecacheComponentData().IsReadyForProxyCreation())
						{
							std::lock_guard Lock(Capture->Mutex);
							Capture->State = bPending ? EThumbnailCaptureState::Preparing : EThumbnailCaptureState::Failed;
							if (!bPending) Capture->Error = Primitive->GetPSOPrecacheDiagnostic().empty()
								? "Thumbnail component PSO preparation failed." : Primitive->GetPSOPrecacheDiagnostic();
							return {};
						}
					}
			}
		}
		IRendererModule* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
		FSceneInterface* Scene = Impl->Scene ? Impl->Scene->GetRenderScene() : nullptr;
		FTextureRHIRef RenderTarget = Impl->RenderTarget;
		const FSceneView View = BuildView(Impl->Output, Impl->View);
		const auto ImageRenderer = Impl->ImageRenderer;
		const auto Output = Impl->Output;
		auto Shaders = ImageRenderer
			? FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"}, EShaderBuildPriority::High)
			: Renderer ? Renderer->PrepareSceneThumbnailShaders() : FShaderMapPreparation{};
		{
			std::lock_guard Lock(Capture->Mutex);
			Capture->Shaders = Shaders;
		}
		FSceneViewRenderOptions Options{
			.Environment = Impl->Environment
		};
		if (Options.Environment)
			Options.Environment->Rotation = Math::Normalize(Options.Environment->Rotation);
		ENQUEUE_RENDER_COMMAND(RenderAssetThumbnailPreview)(
			[Capture, Generation, Renderer, Scene, RenderTarget, View, Options, ImageRenderer, Output, Preparation = Capture->Preparation, Shaders = std::move(Shaders)](
				FRHICommandListImmediate& CommandList
			) mutable {
				std::shared_ptr<FRHITextureReadback> Readback;
				FPreparedShaderMapScope PreparedShaders;
				std::string Error;
				auto ShaderReady = Shaders.Poll();
				bool bPending = ShaderReady && !*ShaderReady;
				if (!ShaderReady) Error = ShaderReady.error();
				else if (!bPending && ImageRenderer)
				{
					FRenderPipelinePreparationBatch Batch;
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					auto Image = ImageRenderer(CommandList, Output.Width, Output.Height);
					bPending = FRenderPipelinePreparationBatch::HasPending();
					if (Image && Image->GetSizeX() == Output.Width && Image->GetSizeY() == Output.Height
						&& Image->GetFormat() == EPixelFormat::SRGBA8_UNORM)
						Readback = CommandList.EnqueueTextureReadback(Image);
					if (!Readback && !bPending) Error = "Thumbnail image rendering or readback failed.";
				}
				else if (!bPending && (Renderer == nullptr || Scene == nullptr || RenderTarget == nullptr))
				{
					Error = "Rendered-thumbnail resources were released before capture.";
				}
				else if (!bPending)
				{
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					FSceneViewStatistics Statistics;
					const FSceneViewRenderRequest Request{Scene, View, RenderTarget, false, Options, &Statistics};
					const auto Progress = Renderer->PrepareViewResources(CommandList, Request, Preparation);
					ERenderViewResult RenderResult = Progress.State == ERenderResourcePreparationState::Ready
						? Renderer->RenderView(CommandList, Request)
						: Progress.State == ERenderResourcePreparationState::Pending
							? ERenderViewResult::RendererResourcesPending : ERenderViewResult::RendererResourcesUnavailable;
					if (Progress.State == ERenderResourcePreparationState::Failed && Options.Environment)
					{
						const auto& Reference = Options.Environment->TextureReference;
						auto* Environment = Reference ? Reference->GetReferencedTexture_RenderThread() : nullptr;
						if (!Environment || Environment->GetDimension() != ETextureDimension::TextureCube)
							RenderResult = ERenderViewResult::RequiredEnvironmentUnavailable;
					}
					bPending = RenderResult == ERenderViewResult::RendererResourcesPending;
					if (!bPending && RenderResult != ERenderViewResult::Success)
						Error = GetRenderFailureDiagnostic(RenderResult);
					else if (!bPending && HasRejectedVisibleGeometry(Statistics))
						Error = "The rendered-thumbnail scene contained visible geometry, "
							"but the renderer produced no geometry draw calls.";
					else if (!bPending) Readback = CommandList.EnqueueTextureReadback(RenderTarget);
				}
				std::lock_guard Lock(Capture->Mutex);
				if (Capture->Generation != Generation)
				{
					if (Readback) Readback->Cancel();
					return;
				}
				Capture->Readback = std::move(Readback);
				Capture->Error = std::move(Error);
				Capture->State = bPending ? EThumbnailCaptureState::Preparing
					: Capture->Error.empty() ? EThumbnailCaptureState::ReadbackPending : EThumbnailCaptureState::Failed;
				if (!bPending)
				{
					Capture->Shaders = {};
					Capture->Preparation = {};
				}
			}
		);
		return {};
	}

	auto FThumbnailCapture::PollCapture() -> std::expected<std::optional<FByteBuffer>, std::string>
	{
		checkf(IsInGameThread(), "Rendered thumbnail capture polling must run on the game thread.");
		std::unique_lock Lock(Impl->Capture->Mutex);
		if (Impl->Capture->State == EThumbnailCaptureState::Preparing)
		{
			Lock.unlock();
			if (auto Retry = BeginCapture(); !Retry) return std::unexpected(Retry.error());
			return std::optional<FByteBuffer>{};
		}
		if (Impl->Capture->State == EThumbnailCaptureState::ReadbackPending)
		{
			const auto State = Impl->Capture->Readback->GetState();
			if (State == ERHITextureReadbackState::Ready)
			{
				if (Impl->Capture->Readback->TakePixels(Impl->Capture->Pixels))
					Impl->Capture->State = EThumbnailCaptureState::Ready;
				else
				{
					Impl->Capture->Error = "The thumbnail readback completed without pixels.";
					Impl->Capture->State = EThumbnailCaptureState::Failed;
				}
			}
			else if (State == ERHITextureReadbackState::Pending)
			{
				ENQUEUE_RENDER_COMMAND(PollThumbnailReadback)([](FRHICommandListImmediate& Commands) {
					Commands.PollTextureReadbacks();
				});
			}
			else
			{
				Impl->Capture->Error = "Failed to read back the rendered-thumbnail output.";
				Impl->Capture->State = EThumbnailCaptureState::Failed;
			}
		}
		if (Impl->Capture->State == EThumbnailCaptureState::Ready)
		{
			Impl->Capture->State = EThumbnailCaptureState::Idle;
			return std::move(Impl->Capture->Pixels);
		}
		else if (Impl->Capture->State == EThumbnailCaptureState::Failed)
			return std::unexpected(Impl->Capture->Error);
		return std::nullopt;
	}

	auto FThumbnailCapture::Reset() -> void
	{
		checkf(IsInGameThread(), "Rendered thumbnail scene reset must run on the game thread.");
		{
			std::lock_guard Lock(Impl->Capture->Mutex);
			++Impl->Capture->Generation;
			Impl->Capture->Shaders = {};
			Impl->Capture->Preparation.RequestCancel();
			Impl->Capture->Preparation = {};
			if (Impl->Capture->Readback) Impl->Capture->Readback->Cancel();
			Impl->Capture->Readback.reset();
			Impl->Capture->State = EThumbnailCaptureState::Idle;
			Impl->Capture->Pixels.clear();
			Impl->Capture->Error.clear();
		}
		Impl->View = MakeDefaultPreviewView();
		Impl->Environment.reset();
		Impl->ImageRenderer = {};
		if (Impl->Scene && Impl->Scene->BindingOwner == this) Impl->Scene->BindingOwner = nullptr;
		Impl->Scene = nullptr;
	}
} // namespace Durin
