#pragma once

#include "Client/Viewport.h"
#include "Rendering/RenderingCommon.h"

class FViewportClient;
class MViewport;

class ENGINE_API FSceneViewport : public FViewport, public IMonaViewport
{
public:
	FSceneViewport(FViewportClient* ViewportClient, TSharedPtr<MViewport> ViewportWidget);

	virtual auto UpdateRHIViewport() -> void override;

private:
	TWeakPtr<MViewport> ViewportWidget_;
};
