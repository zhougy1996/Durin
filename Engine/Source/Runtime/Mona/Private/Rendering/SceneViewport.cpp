#include "Rendering/SceneViewport.h"

#include "Viewport.h"
#include "Widgets/KWindow.h"
#include "Widgets/KViewport.h"
#include "Application/MonaApplication.h"


FSceneViewport::FSceneViewport(FViewportClient* ViewportClient, TSharedPtr<KViewport> ViewportWidget)
	: FViewport(ViewportClient)
	, ViewportWidget_(ViewportWidget)
{
}

auto FSceneViewport::UpdateRHIViewport() -> void
{
	TSharedPtr<KWidget> ViewportWidget = ViewportWidget_.lock();
	TSharedPtr<KWindow> Window = FKleeApplication::Get().FindWidgetWindow(ViewportWidget);
}
