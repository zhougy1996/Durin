#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"
#include "RenderingThread.h"

#include <ranges>

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
		FVector2f ViewportSize = Window->GetViewportSize();

		int32 Width = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.x));
		int32 Height = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.y));

		std::shared_ptr<FGlfwWindow> GLFWWindow = std::dynamic_pointer_cast<FGlfwWindow>(Window->GetNativeWindow());

		bool bFullScreen = false;

		if (Window->GetWindowMode() == EWindowMode::Fullscreen)
		{
			bFullScreen = true;
		}

		auto* ViewportInfo = new FMonaViewportInfo();
		ViewportInfo->ViewportRHI = GDynamicRHI->RHICreateViewport(GLFWWindow->GetOSNativeWindowHandle(), Width, Height, bFullScreen, EPixelFormat::SRGBA8_UNORM);
		ViewportInfo->bFullScreen = bFullScreen;
		WindowToViewportInfoMap.emplace(Window.get(), ViewportInfo);
	}

	auto FMonaRHIRenderer::RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void
	{
		auto ViewportInfoIt = WindowToViewportInfoMap.find(Window.get());
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			FMonaViewportInfo* ViewportInfo = ViewportInfoIt->second;
			GDynamicRHI->RHIResizeViewport(ViewportInfo->ViewportRHI.GetReference(), Width, Height, ViewportInfo->bFullScreen);
		}
	}

	auto FMonaRHIRenderer::DrawWindows() -> void
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

} // namespace Doge::Mona