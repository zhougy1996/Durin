#pragma once

#include "KWidget.h"

class KLEE_API KViewport : public KWidget
{
public:
	KViewport() = default;
	virtual ~KViewport() = default;
	virtual auto DrawWidget() -> void override {}
};
