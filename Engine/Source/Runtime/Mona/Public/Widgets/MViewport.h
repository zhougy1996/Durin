#pragma once

#include "Widgets/MWidget.h"

namespace Doge
{
	class MONA_API MViewport : public MWidget
	{
	public:
		MViewport() = default;
		virtual ~MViewport() = default;
		virtual auto DrawWidget() -> void override {}
	};
}