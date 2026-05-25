#pragma once

#include "Widgets/MWidget.h"

namespace Durin::Mona
{
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
