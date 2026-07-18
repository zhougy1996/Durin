#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Widgets/MWindow.h"

namespace Durin::Mona
{
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
		ViewportInfo->ViewportRHI = GDynamicRHI->RHICreateViewport(
			PlatformWindow->GetOSNativeWindowHandle(),
			Width,
			Height,
			bFullScreen,
			EPixelFormat::SRGBA8_UNORM,
			Window->GetViewportPresentModePolicy()
		);
		ViewportInfo->bFullScreen = bFullScreen;
		ViewportInfo->CurrentWidth = Width;
		ViewportInfo->CurrentHeight = Height;
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
			if (ViewportInfo->CurrentWidth == ClampedWidth && ViewportInfo->CurrentHeight == ClampedHeight)
			{
				return;
			}

			ViewportInfo->PendingResizeWidth = ClampedWidth;
			ViewportInfo->PendingResizeHeight = ClampedHeight;
			ViewportInfo->bResizeRequested = true;
		}
	}

	auto FMonaRHIRenderer::RenderViewports() -> void
	{
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

	auto FMonaRHIRenderer::GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport>
	{
		const auto ViewportInfoIt = WindowToViewportInfoMap.find(&Window);
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			FMonaViewportInfo* ViewportInfo = ViewportInfoIt->second;
			if (ViewportInfo->bResizeRequested)
			{
				ViewportInfo->bResizeRequested = false;
				GDynamicRHI->RHIResizeViewport(
					ViewportInfo->ViewportRHI.GetReference(),
					ViewportInfo->PendingResizeWidth,
					ViewportInfo->PendingResizeHeight,
					ViewportInfo->bFullScreen
				);
				ViewportInfo->CurrentWidth = ViewportInfo->PendingResizeWidth;
				ViewportInfo->CurrentHeight = ViewportInfo->PendingResizeHeight;
			}
			return ViewportInfo->ViewportRHI;
		}
		return nullptr;
	}

} // namespace Durin::Mona
