#include "Mona/SceneViewport.h"

#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"
#include "Widgets/MViewport.h"
#include "Application/MonaApplication.h"

namespace Durin
{
	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MWindow>& InWindow)
		: FViewport(InViewportClient)
		, RenderMode(Mona::EMonaViewportRenderMode::Window)
		, Window(InWindow)
	{
	}

	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MViewport>& InViewportWidget)
		: FViewport(InViewportClient)
		, RenderMode(Mona::EMonaViewportRenderMode::RenderTarget)
		, ViewportWidget(InViewportWidget)
	{
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		if (RenderMode == Mona::EMonaViewportRenderMode::Window)
		{
			const std::shared_ptr<Mona::MWindow> WindowPtr = Window.lock();
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

		ViewportRHI = nullptr;
	}

	auto FSceneViewport::GetRenderMode() const -> Mona::EMonaViewportRenderMode
	{
		return RenderMode;
	}

	auto FSceneViewport::IsWindowBacked() const -> bool
	{
		return RenderMode == Mona::EMonaViewportRenderMode::Window;
	}

	auto FSceneViewport::GetDesiredSize() const -> FVector2f
	{
		if (RenderMode == Mona::EMonaViewportRenderMode::Window)
		{
			if (const std::shared_ptr<Mona::MWindow> WindowPtr = Window.lock())
			{
				return WindowPtr->GetViewportSize();
			}
			return {};
		}

		if (const std::shared_ptr<Mona::MViewport> ViewportWidgetPtr = ViewportWidget.lock())
		{
			return ViewportWidgetPtr->GetDesiredSize();
		}

		return {};
	}
}
