#pragma once
#include "Viewport.h"

#include "Rendering/RenderingCommon.h"

class FViewportClient;
class KViewport;

class KLEE_API FSceneViewport : public FViewport, public IKleeViewport
{
public:
	FSceneViewport(FViewportClient* ViewportClient, TSharedPtr<KViewport> ViewportWidget);

	virtual auto UpdateRHIViewport() -> void override;

private:
	TWeakPtr<KViewport> ViewportWidget_;
};
