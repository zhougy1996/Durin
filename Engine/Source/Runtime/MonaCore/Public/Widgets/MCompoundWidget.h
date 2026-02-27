#pragma once

#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	protected:
		auto OnRender() -> void override;

		TSharedPtr<MWidget> ChildWidget;
	};
}