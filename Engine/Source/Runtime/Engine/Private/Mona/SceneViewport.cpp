#include "Mona/SceneViewport.h"

#include "Widgets/MWindow.h"
#include "Widgets/MViewport.h"
#include "Application/MonaApplication.h"

namespace Doge
{
	FSceneViewport::FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MViewport>& InViewportWidget)
		: FViewport(InViewportClient)
		, ViewportWidget(InViewportWidget)
	{
	}

	auto FSceneViewport::UpdateRHIViewport() -> void
	{
		std::shared_ptr<Mona::MWindow> Window = Mona::FMonaApplication::Get().FindWidgetWindow(ViewportWidget.lock());
	}
}