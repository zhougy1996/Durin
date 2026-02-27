#include "Mona/SceneViewport.h"

#include "Widgets/MWindow.h"
#include "Widgets/MViewport.h"
#include "Application/MonaApplication.h"

namespace Doge
{
	FSceneViewport::FSceneViewport(FViewportClient* ViewportClient, TSharedPtr<Mona::MViewport> ViewportWidget)
		: FViewport(ViewportClient)
		, ViewportWidget_(ViewportWidget)
	{
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		TSharedPtr<Mona::MWidget> ViewportWidget = ViewportWidget_.lock();
		TSharedPtr<Mona::MWindow> Window = Mona::FMonaApplication::Get().FindWidgetWindow(ViewportWidget);
	}
}