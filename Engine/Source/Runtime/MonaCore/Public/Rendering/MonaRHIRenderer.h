#pragma once

#include "Rendering/MonaRenderer.h"

class FRHIViewport;
class MWindow;

class MONA_CORE_API FMonaRHIRenderer : public FMonaRenderer
{
public:
	virtual auto GetRHIViewport(MWindow& Window) -> TSharedPtr<FRHIViewport>;

	virtual auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void;
};