#include "Client/SceneViewport.h"

#include "Application/MonaApplication.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "IRendererModule.h"

namespace Durin
{
	auto FSceneViewport::CreateWindowBacked(
		FViewportClient* InViewportClient,
		const std::shared_ptr<MWindow>& InWindow) -> std::shared_ptr<FSceneViewport>
	{
		return std::shared_ptr<FSceneViewport>(new FSceneViewport(InViewportClient, InWindow));
	}

	auto FSceneViewport::CreateOffscreen(
		FViewportClient* InViewportClient,
		FSceneInterface* InRenderScene) -> std::shared_ptr<FSceneViewport>
	{
		return std::shared_ptr<FSceneViewport>(new FSceneViewport(InViewportClient, InRenderScene));
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow)
		: FViewport(InViewportClient)
		, OutputPolicy(EOutputPolicy::Window)
		, Window(InWindow)
	{
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, FSceneInterface* InRenderScene)
		: FViewport(InViewportClient)
		, OutputPolicy(EOutputPolicy::Offscreen)
		, RenderScene(InRenderScene)
	{
	}

	auto FSceneViewport::InitializeViewState(
		IRendererModule* RendererModule) -> void
	{
		if (!ViewStateOwner && RendererModule != nullptr)
			ViewStateOwner = RendererModule->CreateViewState();
	}

	auto FSceneViewport::ReleaseViewState() -> void
	{
		ViewStateOwner.Reset();
	}

	auto FSceneViewport::RequestHistoryReset() const -> void
	{
		bHistoryResetRequested = true;
	}

	auto FSceneViewport::ConsumeHistoryReset() const -> bool
	{
		const bool bRequested = bHistoryResetRequested;
		bHistoryResetRequested = false;
		return bRequested;
	}

	auto FSceneViewport::PrepareDisplay(const FVector2f& DesiredSize) -> void
	{
		check(!IsWindowBacked());
		OffscreenExtent = SanitizeDisplayExtent(DesiredSize);
		UpdateRHIViewport();
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		if (IsWindowBacked())
		{
			const std::shared_ptr<MWindow> WindowPtr = Window.lock();
			if (WindowPtr == nullptr)
			{
				ViewportRHI = nullptr;
				return;
			}

			if (Mona::FMonaRenderer* Renderer = Mona::FMonaApplication::Get().GetRenderer())
			{
				ViewportRHI = Renderer->PrepareViewportForDraw(*WindowPtr);
			}
			return;
		}

		const uint32 Width = static_cast<uint32>(OffscreenExtent.x);
		const uint32 Height = static_cast<uint32>(OffscreenExtent.y);
		if (RenderTargetRHI == nullptr || RenderTargetRHI->GetSizeX() != Width || RenderTargetRHI->GetSizeY() != Height)
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
				"SceneViewportRenderTarget", Width, Height, EPixelFormat::SRGBA8_UNORM);
			Desc.AddFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			RenderTargetRHI = RHICreateTexture(Desc);
			if (RenderTargetRHI != nullptr)
			{
				const FTextureRHIRef NewRenderTarget = RenderTargetRHI;
				ENQUEUE_RENDER_COMMAND(InitializeSceneViewportRenderTarget)(
					[NewRenderTarget](FRHICommandListImmediate& CommandList) {
						const std::array Transition{FRHITextureTransition::Whole(
							NewRenderTarget, ERHIAccess::Discard,
							ERHIAccess::GraphicsShaderRead)};
						CommandList.TransitionTextures(Transition);
					});
			}
		}
		ViewportRHI = nullptr;
	}

	auto FSceneViewport::IsWindowBacked() const -> bool
	{
		return OutputPolicy == EOutputPolicy::Window;
	}

	auto FSceneViewport::GetDesiredSize() const -> FVector2f
	{
		if (IsWindowBacked())
		{
			if (const std::shared_ptr<MWindow> WindowPtr = Window.lock())
			{
				return SanitizeDisplayExtent(WindowPtr->GetViewportSize());
			}
			return SanitizeDisplayExtent({});
		}
		return OffscreenExtent;
	}

	auto FSceneViewport::GetRenderTargetRHI() const -> const FTextureRHIRef&
	{
		return RenderTargetRHI;
	}

	auto FSceneViewport::GetDisplayTexture() const -> const FTextureRHIRef&
	{
		return RenderTargetRHI;
	}

	auto FSceneViewport::PublishRenderStatistics_RenderThread(
		const FSceneViewStatistics& Statistics,
		bool bAvailable) -> void
	{
		check(IsInRenderingThread());
		std::scoped_lock Lock(StatisticsMutex);
		if (StatisticsSnapshot.Revision != std::numeric_limits<uint64>::max())
			++StatisticsSnapshot.Revision;
		StatisticsSnapshot.bAvailable = bAvailable;
		StatisticsSnapshot.Statistics = bAvailable
			? Statistics
			: FSceneViewStatistics{};
	}

	auto FSceneViewport::GetRenderStatisticsSnapshot() const
		-> FSceneViewportStatisticsSnapshot
	{
		std::scoped_lock Lock(StatisticsMutex);
		return StatisticsSnapshot;
	}

	auto FSceneViewport::RequestRenderGraphCapture() -> void
	{
		bRenderGraphCaptureRequested.store(true, std::memory_order_release);
	}

	auto FSceneViewport::ConsumeRenderGraphCaptureRequest() -> bool
	{
		return bRenderGraphCaptureRequested.exchange(
			false, std::memory_order_acq_rel);
	}

	auto FSceneViewport::PublishRenderGraphCapture_RenderThread(
		std::shared_ptr<const FRDGCapture> Capture,
		bool bAvailable) -> void
	{
		check(IsInRenderingThread());
		std::scoped_lock Lock(RenderGraphMutex);
		if (RenderGraphSnapshot.Revision != std::numeric_limits<uint64>::max())
			++RenderGraphSnapshot.Revision;
		RenderGraphSnapshot.bAvailable = bAvailable && Capture != nullptr;
		RenderGraphSnapshot.Capture = RenderGraphSnapshot.bAvailable
			? std::move(Capture) : nullptr;
	}

	auto FSceneViewport::GetRenderGraphSnapshot() const
		-> FSceneViewportRenderGraphSnapshot
	{
		std::scoped_lock Lock(RenderGraphMutex);
		return RenderGraphSnapshot;
	}

	auto FSceneViewport::SanitizeDisplayExtent(const FVector2f& DesiredSize) -> FVector2f
	{
		return {
			static_cast<float>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x))),
			static_cast<float>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)))
		};
	}
}
