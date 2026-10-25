#pragma once

#include "CoreMinimal.h"

#include "Client/Viewport.h"
#include "Rendering/ViewportDisplaySource.h"
#include "RDG/RDGDiagnostics.h"
#include "ViewRenderStatistics.h"
#include "SceneViewState.h"
#include "RenderResourcePreparation.h"

#include "MonaCoreFwd.h"

namespace Durin
{
	class FRHICommandListImmediate;
	struct FSceneView;
	struct FSceneViewRenderOptions;
	class FViewportClient;
	class IRendererModule;
	struct FSceneViewRenderRequest;
	enum class ERenderViewResult : uint8;
	class FSceneInterface;
	class MWindow;

	// Identifies the latest complete render result published for one scene viewport.
	struct FSceneViewportStatisticsSnapshot
	{
		FSceneViewStatistics Statistics;
		uint64 Revision = 0;
		bool bAvailable = false;

		auto operator==(const FSceneViewportStatisticsSnapshot&) const
			-> bool = default;
	};

	// Identifies the latest explicitly captured graph for one scene viewport.
	struct FSceneViewportRenderGraphSnapshot
	{
		std::shared_ptr<const FRDGCapture> Capture;
		uint64 Revision = 0;
		bool bAvailable = false;
	};

	// Owns Engine window or offscreen output while exposing only its offscreen texture through MonaCore.
	class FSceneViewport final : public FViewport, public IViewportDisplaySource
	{
	public:
		ENGINE_API static auto CreateWindowBacked(
			FViewportClient* InViewportClient,
			const std::shared_ptr<MWindow>& InWindow) -> std::shared_ptr<FSceneViewport>;

		ENGINE_API static auto CreateOffscreen(
			FViewportClient* InViewportClient,
			FSceneInterface* InRenderScene = nullptr) -> std::shared_ptr<FSceneViewport>;

		~FSceneViewport() override = default;

		// Attaches this logical viewport to the active renderer when available.
		ENGINE_API auto InitializeViewState(IRendererModule* RendererModule) -> void;
		ENGINE_API auto ReleaseViewState() -> void;
		auto GetViewStateId() const -> FSceneViewStateId
		{
			return ViewStateOwner.GetId();
		}
		// Camera possession, teleports, cuts, and scene lifecycle owners call this explicitly.
		ENGINE_API auto RequestHistoryReset() const -> void;
		ENGINE_API auto ConsumeHistoryReset() const -> bool;

		// Explicit loading barrier; later streamed combinations use demand preparation.
		auto RequestResourcePreparation() -> void { bResourcePreparationRequested = true; }
		ENGINE_API auto RenderView_RenderThread(IRendererModule& Renderer, FRHICommandListImmediate& Commands,
			FSceneViewRenderRequest Request) -> ERenderViewResult;
		auto GetResourcePreparation_RenderThread() const -> const FRenderResourcePreparation& { return ResourcePreparation; }
		auto GetResourcePreparationProgress_RenderThread() const -> FRenderResourcePreparationProgress
		{ return ResourcePreparation.GetProgress(); }
		auto SetSceneGeneration(uint64 Generation) -> void
		{ SceneGeneration = Generation; OutputReceipt = std::make_shared<FViewportOutputReceipt>(); bRenderFailed = false; }
		auto GetSceneGeneration() const -> uint64 { return SceneGeneration; }
		auto GetOutputReceipt() const -> std::shared_ptr<FViewportOutputReceipt> override { return OutputReceipt; }
		auto OnDisplaySubmitted() -> void override { bDisplayRequested = true; }
		auto ConsumeDisplayRequest() -> bool { return std::exchange(bDisplayRequested, false); }
		auto HasRenderFailed() const -> bool { return bRenderFailed.load(); }
		ENGINE_API auto PublishRenderOutcome_RenderThread(ERenderViewResult Result) -> void;

		ENGINE_API auto PrepareDisplay(const FVector2f& DesiredSize) -> void override;
		ENGINE_API auto GetDisplayTexture() const -> const FTextureRHIRef& override;

		ENGINE_API auto UpdateRHIViewport() -> void override;
		ENGINE_API auto IsWindowBacked() const -> bool;
		ENGINE_API auto GetDesiredSize() const -> FVector2f;
		ENGINE_API auto GetRenderTargetRHI() const -> const FTextureRHIRef&;
		// Publishes one render-thread result without retaining partial failure data.
		ENGINE_API auto PublishRenderStatistics_RenderThread(
			const FSceneViewStatistics& Statistics,
			bool bAvailable) -> void;
		ENGINE_API auto GetRenderStatisticsSnapshot() const
			-> FSceneViewportStatisticsSnapshot;
		// Requests one full owning graph capture from the next submitted frame.
		ENGINE_API auto RequestRenderGraphCapture() -> void;
		ENGINE_API auto ConsumeRenderGraphCaptureRequest() -> bool;
		ENGINE_API auto PublishRenderGraphCapture_RenderThread(
			std::shared_ptr<const FRDGCapture> Capture,
			bool bAvailable) -> void;
		ENGINE_API auto GetRenderGraphSnapshot() const
			-> FSceneViewportRenderGraphSnapshot;

		// Auxiliary editor viewports may render an isolated scene instead of leaking preview primitives into the level.
		ENGINE_API auto GetRenderScene() const -> FSceneInterface* { return RenderScene; }

	private:
		enum class EOutputPolicy
		{
			Window,
			Offscreen
		};

		FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow);
		FSceneViewport(FViewportClient* InViewportClient, FSceneInterface* InRenderScene);

		static auto SanitizeDisplayExtent(const FVector2f& DesiredSize) -> FVector2f;

		std::atomic_bool bResourcePreparationRequested = false;
		uint64 SceneGeneration = 0;
		std::shared_ptr<FViewportOutputReceipt> OutputReceipt = std::make_shared<FViewportOutputReceipt>();
		std::atomic_bool bRenderFailed = false;
		bool bDisplayRequested = false;
		FRenderResourcePreparation ResourcePreparation;
		EOutputPolicy OutputPolicy = EOutputPolicy::Window;
		std::weak_ptr<MWindow> Window;
		FVector2f OffscreenExtent = {640.0f, 360.0f};
		FTextureRHIRef RenderTargetRHI;
		FSceneInterface* RenderScene = nullptr;
		FSceneViewStateOwner ViewStateOwner;
		mutable bool bHistoryResetRequested = true;
		mutable std::mutex StatisticsMutex;
		FSceneViewportStatisticsSnapshot StatisticsSnapshot;
		std::atomic_bool bRenderGraphCaptureRequested = false;
		mutable std::mutex RenderGraphMutex;
		FSceneViewportRenderGraphSnapshot RenderGraphSnapshot;
	};
}
