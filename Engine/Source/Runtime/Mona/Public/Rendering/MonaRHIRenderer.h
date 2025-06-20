#pragma once

#include "Rendering/MonaRenderer.h"

class FRHIViewport;
class KWindow;

class KLEE_API FKleeRHIRenderer : public FKleeRenderer
{
public:
	virtual auto GetRHIViewport(KWindow& Window) -> TSharedPtr<FRHIViewport>;

	virtual auto CreateViewport(const TSharedPtr<KWindow>& Window) -> void;
};