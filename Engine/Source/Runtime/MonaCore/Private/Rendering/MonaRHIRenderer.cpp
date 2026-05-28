#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "Widgets/MWindow.h"

namespace Durin::Mona
{
	constexpr int32 MIN_VIEWPORT_SIZE = 8;

	FMonaRHIRenderer::~FMonaRHIRenderer()
	{
		for (const auto& Info : WindowToViewportInfoMap | std::views::values)
		{
			delete Info;
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
		ViewportInfo->ViewportRHI = GDynamicRHI->RHICreateViewport(PlatformWindow->GetOSNativeWindowHandle(), Width, Height, bFullScreen, EPixelFormat::SRGBA8_UNORM);
		ViewportInfo->bFullScreen = bFullScreen;
		WindowToViewportInfoMap.emplace(Window.get(), ViewportInfo);
	}

	auto FMonaRHIRenderer::RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void
	{
		auto ViewportInfoIt = WindowToViewportInfoMap.find(Window.get());
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			FMonaViewportInfo* ViewportInfo = ViewportInfoIt->second;
			const uint32 ClampedWidth = static_cast<uint32>(FMath::Max(MIN_VIEWPORT_SIZE, static_cast<int32>(Width)));
			const uint32 ClampedHeight = static_cast<uint32>(FMath::Max(MIN_VIEWPORT_SIZE, static_cast<int32>(Height)));
			GDynamicRHI->RHIResizeViewport(ViewportInfo->ViewportRHI.GetReference(), ClampedWidth, ClampedHeight, ViewportInfo->bFullScreen);
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
			delete it->second;
			WindowToViewportInfoMap.erase(Window.get());
		}
	}

	auto FMonaRHIRenderer::GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport>
	{
		const auto ViewportInfoIt = WindowToViewportInfoMap.find(&Window);
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			return ViewportInfoIt->second->ViewportRHI;
		}
		return nullptr;
	}

} // namespace Durin::Mona
