#include "Rendering/MonaRHIRenderer.h"

#include "DynamicRHI.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"

auto FMonaRHIRenderer::GetRHIViewport(MWindow& Window) -> TSharedPtr<FRHIViewport>
{
	return Window.GetRHIViewport();
}

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

	TSharedPtr<FRHIViewport> RHIViewport = GDynamicRHI->RHICreateViewport(GLFWWindow->GetOSNativeWindowHandle(), Width, Height, bFullScreen, EPixelFormat::R8G8B8A8);

	Window->SetRHIViewport(RHIViewport);
}
