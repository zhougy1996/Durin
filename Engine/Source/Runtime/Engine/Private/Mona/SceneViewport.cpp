#include "Mona/SceneViewport.h"

#include "Widgets/MWindow.h"
#include "Widgets/MViewport.h"
#include "Application/MonaApplication.h"

namespace Doge
{
	FSceneViewport::FSceneViewport(FViewportClient* ViewportClient, TSharedPtr<MViewport> ViewportWidget)
		: FViewport(ViewportClient)
		, ViewportWidget_(ViewportWidget)
	{
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		TSharedPtr<MWidget> ViewportWidget = ViewportWidget_.lock();
		TSharedPtr<MWindow> Window = FMonaApplication::Get().FindWidgetWindow(ViewportWidget);
	}
}