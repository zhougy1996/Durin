#pragma once

#include "Widgets/MWidget.h"

namespace Durin
{
	// Adapts a callback into a lightweight drawable Mona widget.
	class MFunctionWidget : public MWidget
	{
	public:
		MFunctionWidget() = default;

		auto Construct(std::function<void()> InOnDraw) -> void
		{
			OnDraw = std::move(InOnDraw);
		}

		auto Draw() -> void override
		{
			if (OnDraw)
			{
				OnDraw();
			}
		}

	private:
		std::function<void()> OnDraw;
	};
}
