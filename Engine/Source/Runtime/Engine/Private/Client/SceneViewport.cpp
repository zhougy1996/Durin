#include "Client/SceneViewport.h"

#include "Application/MonaApplication.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

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
		IScene* InRenderScene) -> std::shared_ptr<FSceneViewport>
	{
		return std::shared_ptr<FSceneViewport>(new FSceneViewport(InViewportClient, InRenderScene));
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow)
		: FViewport(InViewportClient)
		, OutputPolicy(EOutputPolicy::Window)
		, Window(InWindow)
	{
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, IScene* InRenderScene)
		: FViewport(InViewportClient)
		, OutputPolicy(EOutputPolicy::Offscreen)
		, RenderScene(InRenderScene)
	{
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
				ViewportRHI = Renderer->GetRHIViewport(*WindowPtr);
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

	auto FSceneViewport::SanitizeDisplayExtent(const FVector2f& DesiredSize) -> FVector2f
	{
		return {
			static_cast<float>(FMath::Max(8, FMath::CeilToInt(DesiredSize.x))),
			static_cast<float>(FMath::Max(8, FMath::CeilToInt(DesiredSize.y)))
		};
	}
}
