#include "Renderers/SceneRenderingService.h"
#include "UniformBuffer.h"
#include "Materials/MaterialRenderProxy.h"
#include "Renderers/MeshPassProcessor.h"

#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "Console/ConsoleCommand.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Rendering/SceneInterface.h"
#include "RHICapabilities.h"

namespace Durin
{
	FSceneRenderingService::FSceneRenderingService()
		: RDGAllocator(Coordinator)
		, DefaultTextures(Coordinator)
		, EnvironmentLighting(Coordinator, Retirement)
		, SurfaceMaterials(Coordinator, DefaultTextures)
		, DirectionalShadowRenderer(Coordinator)
		, GBufferRenderer(Coordinator)
		, InstanceCullingManager(Coordinator)
		, GBufferDebugRenderer(Coordinator, FullscreenGeometry)
		, DeferredDirectionalLightingRenderer(Coordinator, FullscreenGeometry)
		, GroundTruthAmbientOcclusionRenderer(Coordinator, FullscreenGeometry)
		, StaticMeshRenderer(Coordinator, SurfaceMaterials)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, ContactShadowRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudShadowRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderingService::~FSceneRenderingService() = default;

	auto FSceneRenderingService::GetPreparationConfig_GameThread(ERHIFeatureLevel FeatureLevel) const -> FSceneRenderPreparationConfig
	{
		CheckGameThread();
		if (FeatureLevel > PreparationConfigGameThread.FeatureLevel) return {};
		auto Config = PreparationConfigGameThread;
		Config.FeatureLevel = FeatureLevel;
		return Config;
	}
	auto FSceneRenderingService::GetPreparationConfig_RenderThread(ERHIFeatureLevel FeatureLevel) const -> FSceneRenderPreparationConfig
	{
		CheckRenderingThread();
		if (FeatureLevel > PreparationConfigRenderThread.FeatureLevel) return {};
		auto Config = PreparationConfigRenderThread;
		Config.FeatureLevel = FeatureLevel;
		return Config;
	}
	auto FSceneRenderingService::SetPreparationSettings_GameThread(const FRendererPreparationSettings& Settings) -> bool
	{
		CheckGameThread();
		if (!PreparationConfigGameThread.Version) return false;
		if (PreparationConfigGameThread.Settings == Settings) return true;
		require(PreparationConfigGameThread.Version != UINT64_MAX);
		PreparationConfigGameThread.Settings = Settings;
		++PreparationConfigGameThread.Version;
		MaterialPSORequests.SetPreparationConfigVersion(PreparationConfigGameThread.Version);
		EnqueueRenderCommand("UpdateRendererPreparationConfig", [this, Config = PreparationConfigGameThread](FRHICommandListImmediate&) {
			PreparationConfigRenderThread = Config;
			MaterialPSORequests.InvalidateOutdatedPreparationConfigs();
			InvalidatePreparationOwners_RenderThread();
			ViewStates.InvalidateAll(ESceneViewDiscontinuity::ManualInvalidation);
		});
		return true;
	}

    auto FSceneRenderingService::BeginFrame_RenderThread(FRHICommandListImmediate& Commands) -> void
    {
        CheckRenderingThread();
        if (GDynamicRHI) Retirement.Collect(GDynamicRHI->RHIGetQueueCapabilities());
		MaterialPSORequests.Pump([this](const auto& Params, auto& Collection, EPSOPrecacheCollectionPhase Phase) {
			// Admission can observe a newer GameThread snapshot before its ordered
			// render command executes. Wait for that version rather than collecting it early.
			if (Params.PreparationConfig.Version != PreparationConfigRenderThread.Version)
				return std::expected<bool, std::string>(false);
			return RendererPrivate::CollectMaterialPSOs(Params, Collection, Coordinator.GetGeneration_RenderThread(), Phase);
		});
        // Updates may requeue themselves while dependencies or GPU work are pending.
        const auto SkyScenes = std::exchange(PendingSkyScenes, {});
        for (auto* Scene : SkyScenes)
        {
            UpdateSkyLighting_RenderThread(Commands,*Scene);
            Scene->SkyLighting->WorldUpdateFrame=GRenderFrameCounterRenderThread;
        }
    }

    auto FSceneRenderingService::UpdateSkyLighting_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
    {
		FPreparedShaderMapScope ShaderScope(true);
        EnvironmentLighting.UpdateScene_RenderThread(Commands, Scene, RDGAllocator, DefaultTextures.GetCube_RenderThread());
        if (Scene.SkyLighting->bUpdatePending) PendingSkyScenes.insert(&Scene);
        else PendingSkyScenes.erase(&Scene);
    }

	auto FSceneRenderingService::ReleaseSceneResources_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
	{
		InvalidatePreparationOwners_RenderThread(&Scene);
		EnvironmentLighting.ReleaseScene_RenderThread(Commands, Scene);
	}

	auto FSceneRenderingService::Start(
		FConsoleCommandRegistry& Registry
	) -> bool
	{
		CheckGameThread();
		PreparationConfigGameThread.FeatureLevel = FSceneInterface::GetDefaultFeatureLevel();
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		PreparationConfigGameThread.bSupportsWireframe = Caps && Caps->bSupportsNonSolidFill;
		PreparationConfigGameThread.Version = 1;
		PreparationConfigRenderThread = PreparationConfigGameThread;
		MaterialPSORequests.SetPreparationConfigVersion(1);
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			}
		);
	}

	auto FSceneRenderingService::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderingService::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList
	) -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.Initialize_RenderThread(CommandList);
	}

	auto FSceneRenderingService::ReleaseResources_RenderThread() -> void
	{
		MaterialPSORequests.Close();
		PSOPrecacheValidation::Clear();
		ReleaseDeviceResources_RenderThread();
		Retirement.Drain([] {
			require(GDynamicRHI != nullptr);
			FRHICommandListImmediate::Get().BlockUntilGPUIdle();
		});
		Coordinator.ReleaseResources_RenderThread();
		VisibilityScratch = {};
	}

	auto FSceneRenderingService::ReleaseDeviceResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		MaterialPSORequests.Invalidate();
		PSOPrecacheValidation::Clear();
		InvalidatePreparationOwners_RenderThread();
		EmptyLocalLights = nullptr;
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread(FRHICommandListImmediate::Get());
		SurfaceMaterials.ReleaseResources_RenderThread();
		StaticMeshRenderer.ReleaseResources_RenderThread();
		RendererPrivate::FCachedMeshDrawCommandStorage::ReleaseRHIForAllResources();
		FMaterialRenderProxy::ReleaseRHIForAllResources();
		FUniformBuffer::ReleaseRHIForAllResources();
		DirectionalShadowRenderer.ReleaseResources_RenderThread();
		GBufferRenderer.ReleaseResources_RenderThread();
		InstanceCullingManager.ReleaseResources_RenderThread();
		GBufferDebugRenderer.ReleaseResources_RenderThread();
		DeferredDirectionalLightingRenderer.ReleaseResources_RenderThread();
		GroundTruthAmbientOcclusionRenderer.ReleaseResources_RenderThread();
		SkyBoxRenderer.ReleaseResources_RenderThread();
		EditorAssistanceRenderer.ReleaseResources_RenderThread();
		PostProcessRenderer.ReleaseResources_RenderThread();
		ContactShadowRenderer.ReleaseResources_RenderThread();
		VolumetricCloudRenderer.ReleaseResources_RenderThread();
		VolumetricCloudShadowRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
		RDGAllocator.Release_RenderThread();
	}

	auto FSceneRenderingService::AddViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Add(Id);
	}

	auto FSceneRenderingService::RemoveViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Remove(Id);
	}

	auto FSceneRenderingService::InvalidateViewState_RenderThread(
		FSceneViewStateId Id
	) -> bool
	{
		return ViewStates.Invalidate(
			Id, ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderingService::InvalidateAllViewStates_RenderThread() -> void
	{
		ViewStates.InvalidateAll(
			ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderingService::ReleaseViewStates_RenderThread() -> size_t
	{
		return ViewStates.ReleaseAll();
	}

	auto FSceneRenderingService::GetViewStateCount_RenderThread() const -> size_t
	{
		check(IsInRenderingThread());
		return ViewStates.Num();
	}

	auto FSceneRenderingService::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			}
		);
	}

	auto FSceneRenderingService::ApplyResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		check(IsInRenderingThread());
		MaterialPSORequests.Invalidate();
		PSOPrecacheValidation::Clear();
		InvalidatePreparationOwners_RenderThread();
		if (Cause == ERendererResourceInvalidationCause::Device)
			ViewStates.InvalidateAll(
				ESceneViewDiscontinuity::DeviceInvalidation
			);
		else RendererPrivate::FCachedMeshDrawCommandStorage::InvalidateMeshPreparationForAllResources();
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool) {},
				.ReleaseDeviceResources =
					[this] {
						ReleaseDeviceResources_RenderThread();
					},
				.RecreateStartupResources =
					[this, &CommandList] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[this, &CommandList] {
						DefaultTextures.Initialize_RenderThread(CommandList);
						FullscreenGeometry.RetryFailedResources_RenderThread();
					},
			}
		);
	}

	auto FSceneRenderingService::InvalidatePreparationOwners_RenderThread(FScene* Scene) -> void
	{
		CheckRenderingThread();
		if (Scene) PreparationOwners.erase(Scene);
		else PreparationOwners.clear();
	}

	auto FSceneRenderingService::PrepareViewResources_RenderThread(
		FRHICommandListImmediate& CommandList, FScene* Scene, const FSceneView& View,
		FRHITexture* OutputTarget, bool bPresentOutput, const FSceneViewRenderOptions& Options,
		const FRenderResourcePreparation& Preparation) -> void
	{
		CheckRenderingThread();
		if (!Preparation.IsValid() || Preparation.GetProgress().State != ERenderResourcePreparationState::Pending) return;
		// An accepted native compile is observed through its shared handle; do not
		// rewalk the scene merely because that task has not completed yet.
		if (!Preparation.NeedsPreparation()) return;
		if (!Scene || !OutputTarget || !GDynamicRHI)
		{
			Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed);
			return;
		}
		auto& Owner = PreparationOwners[Scene];
		if (!Owner) Owner = std::make_shared<const uint8>(0);
		std::vector<std::shared_ptr<FPipelineState>> Requests;
		const auto Result = FSceneRenderPipeline(*this).Execute_RenderThread(CommandList, Scene, View,
			OutputTarget, bPresentOutput, Options, nullptr, nullptr, true, &Requests);
		Preparation.Complete_RenderThread(Result == ERenderViewResult::Success ? ERenderResourcePreparationState::Ready
			: Result == ERenderViewResult::RendererResourcesPending ? ERenderResourcePreparationState::Pending
			: ERenderResourcePreparationState::Failed, std::move(Requests), Owner);
	}

	auto FSceneRenderingService::RenderView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRDGCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		return FSceneRenderPipeline(*this).Execute_RenderThread(
			CommandList, Scene, View, OutputTarget, bPresentOutput,
			Options, OutStatistics, OutRenderGraphCapture
		);
	}

} // namespace Durin
