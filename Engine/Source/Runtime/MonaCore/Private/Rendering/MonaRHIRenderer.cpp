#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"

namespace Doge::Mona
{
	constexpr int32 MIN_VIEWPORT_SIZE = 8;

	auto FMonaRHIRenderer::CreateViewport(const TSharedPtr<MWindow>& Window) -> void
	{
		FVector2f ViewportSize = Window->GetViewportSize();

		int32 Width = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.x));
		int32 Height = FMath::Max(MIN_VIEWPORT_SIZE, FMath::CeilToInt(ViewportSize.y));

		TSharedPtr<FGlfwWindow> GLFWWindow = std::dynamic_pointer_cast<FGlfwWindow>(Window->GetNativeWindow());

		bool bFullScreen = false;
		if (Window->GetWindowMode() == EWindowMode::Fullscreen)
		{
			bFullScreen = true;
		}

		auto* ViewportInfo = new FMonaViewportInfo();
		ViewportInfo->ViewportRHI = GDynamicRHI->RHICreateViewport(GLFWWindow->GetOSNativeWindowHandle(), Width, Height, bFullScreen, EPixelFormat::SRGBA8_UNORM);;
		ViewportInfo->bFullScreen = bFullScreen;
		WindowToViewportInfoMap.emplace(Window.get(), ViewportInfo);
	}

	auto FMonaRHIRenderer::DrawWindows() -> void
	{
	}

	auto FMonaRHIRenderer::GetRHIViewport(const MWindow& Window) -> TSharedPtr<FRHIViewport>
	{
		const auto ViewportInfoIt = WindowToViewportInfoMap.find(&Window);
		if (ViewportInfoIt != WindowToViewportInfoMap.end())
		{
			return ViewportInfoIt->second->ViewportRHI;
		}
		return nullptr;
	}
}