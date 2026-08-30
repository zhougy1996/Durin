#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Widgets/MWindow.h"

namespace Durin::Mona
{
	FMonaRHIRenderer::FMonaRHIRenderer(
		bool bInAdoptInitializationPresentationCandidate)
		: bAdoptInitializationPresentationCandidate(
			bInAdoptInitializationPresentationCandidate)
	{
	}

	constexpr int32 MIN_VIEWPORT_SIZE = 8;

	namespace
	{
		auto ReleaseViewportInfo(FMonaViewportInfo* Info) -> void
		{
			if (!Info) return;
			TRefCountPtr<FRHIViewport> ViewportRHI = std::move(Info->ViewportRHI);
			delete Info;
			if (!ViewportRHI) return;
			if (!GRenderingThread)
			{
				ViewportRHI = nullptr;
				return;
			}
			// Vulkan viewport destruction may wait for presentation. Keep that work off
			// the application thread so closing a PIE window cannot freeze editor input.
			ENQUEUE_RENDER_COMMAND(ReleaseMonaViewport)([ViewportRHI = std::move(ViewportRHI)](FRHICommandListImmediate&) mutable {
				ViewportRHI = nullptr;
			});
		}
	}

	FMonaRHIRenderer::~FMonaRHIRenderer()
	{
		for (const auto& Info : WindowToViewportInfoMap | std::views::values)
		{
			ReleaseViewportInfo(Info);
		}
		WindowToViewportInfoMap.clear();
	}

	auto FMonaRHIRenderer::CreateViewport(const std::shared_ptr<MWindow>& Window) -> void
	{
		if (WindowToViewportInfoMap.contains(Window.get()))
		{
			return;
		}

		FVector2f ViewportSize = Window->GetViewportSize();

		int32 Width = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.x));
		int32 Height = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.y));

		std::shared_ptr<FGenericWindow> PlatformWindow = Window->GetNativeWindow();
		check(PlatformWindow != nullptr);

		bool bFullScreen = false;

		if (Window->GetWindowMode() == EWindowMode::Fullscreen)
		{
			bFullScreen = true;
		}

		auto* ViewportInfo = new FMonaViewportInfo();
		FRHIViewportCreateInfo CreateInfo{
			.NativeWindowHandle = PlatformWindow->GetOSNativeWindowHandle(),
			.SizeX = static_cast<uint32>(Width),
			.SizeY = static_cast<uint32>(Height),
			.bIsFullscreen = bFullScreen,
			.PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM,
			.PresentationPolicy = Window->GetViewportPresentationPolicy(),
			.bAdoptInitializationPresentationCandidate =
				bAdoptInitializationPresentationCandidate};
		ViewportInfo->ViewportRHI =
			GDynamicRHI->RHICreateViewport(CreateInfo);
		if (ViewportInfo->ViewportRHI
			&& CreateInfo.bAdoptInitializationPresentationCandidate)
		{
			bAdoptInitializationPresentationCandidate = false;
		}
		ViewportInfo->bFullScreen = bFullScreen;
		ViewportInfo->SubmittedExtent = {Width, Height};
		WindowToViewportInfoMap.emplace(Window.get(), ViewportInfo);
	}

	auto FMonaRHIRenderer::RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void
	{
		if (Width == 0 || Height == 0)
		{
			return;
		}

		auto ViewportInfoIt = WindowToViewportInfoMap.find(Window.get());
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			FMonaViewportInfo* ViewportInfo = ViewportInfoIt->second;
			const uint32 ClampedWidth = static_cast<uint32>(FMath::Max(MIN_VIEWPORT_SIZE, static_cast<int32>(Width)));
			const uint32 ClampedHeight = static_cast<uint32>(FMath::Max(MIN_VIEWPORT_SIZE, static_cast<int32>(Height)));
			const FIntPoint RequestedExtent{
				static_cast<int32>(ClampedWidth),
				static_cast<int32>(ClampedHeight)};
			ViewportInfo->QueueResize(RequestedExtent);
		}
	}

	auto FMonaRHIRenderer::OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void
	{
		auto it = WindowToViewportInfoMap.find(Window.get());
		if (it != WindowToViewportInfoMap.end())
		{
			ReleaseViewportInfo(it->second);
			WindowToViewportInfoMap.erase(Window.get());
		}
	}

	auto FMonaRHIRenderer::PrepareViewportForDraw(const MWindow& Window) -> TRefCountPtr<FRHIViewport>
	{
		const auto ViewportInfoIt = WindowToViewportInfoMap.find(&Window);
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			FMonaViewportInfo* ViewportInfo = ViewportInfoIt->second;
			if (const std::optional<FIntPoint> PendingExtent = ViewportInfo->TakePendingResize())
			{
				GDynamicRHI->RHIResizeViewport(
					ViewportInfo->ViewportRHI.GetReference(),
					static_cast<uint32>(PendingExtent->x),
					static_cast<uint32>(PendingExtent->y),
					ViewportInfo->bFullScreen
				);
				ViewportInfo->SubmittedExtent = *PendingExtent;
			}
			return ViewportInfo->ViewportRHI;
		}
		return nullptr;
	}

} // namespace Durin::Mona
