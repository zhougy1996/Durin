#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	class FSceneRenderingService;
	enum class ERendererResourceInvalidationCause : uint8;
	struct FConsoleCommandResult;
	struct FRendererResourceInvalidationSnapshot;

	// Adapts the public renderer-module contract to one private scene renderer.
	class FRendererModule final : public IRendererModule
	{
	public:
		RENDERER_API FRendererModule();
		RENDERER_API ~FRendererModule() override;

		RENDERER_API auto StartupModule() -> void override;
		RENDERER_API auto PrecacheMaterialPSOs(FMaterialPSOPrecacheParams Params) -> FMaterialPSOPrecacheRequestID override;
		RENDERER_API auto ShutdownModule() -> void override;
		RENDERER_API auto RenderHitProxies(FRHICommandListImmediate&, FSceneInterface*, const FHitProxyRenderRequest&) -> std::shared_ptr<const FHitProxyMap> override;
		RENDERER_API auto CreateScene() -> FScenePtr override;
		RENDERER_API auto PrepareSceneThumbnailShaders() -> FShaderMapPreparation override;
		RENDERER_API auto BeginFrame_RenderThread(FRHICommandListImmediate& Commands) -> void override;
		RENDERER_API auto SetViewGPUTimingSink_RenderThread(std::function<void(FGPUTimingQueryRHIRef)> Sink) -> void override;
		RENDERER_API auto CreateViewState() -> FSceneViewStateOwner override;
		RENDERER_API auto InvalidateViewState(FSceneViewStateId Id) -> void override;
		RENDERER_API auto InvalidateAllViewStates() -> void override;
		RENDERER_API auto RequestResourceInvalidation(
			ERendererResourceInvalidationCause Cause) -> FConsoleCommandResult;
		RENDERER_API auto GetResourceInvalidationSnapshot_RenderThread() const
			-> FRendererResourceInvalidationSnapshot;
		// Pump on the render thread with the actual upcoming scene/view/output.
		// Pending never joins PSOs or draws; callers retain the handle through activation.
		RENDERER_API auto PrepareViewResources(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request, const FRenderResourcePreparation& Preparation) -> FRenderResourcePreparationProgress override;
		RENDERER_API auto RenderView(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request) -> ERenderViewResult override;

	private:
		static auto ReleaseViewState(FSceneViewStateId Id) -> void;

		std::unique_ptr<FSceneRenderingService> RenderingService;
	};
}
