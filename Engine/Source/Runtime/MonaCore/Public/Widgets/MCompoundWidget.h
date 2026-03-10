#pragma once

#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	public:
		auto Draw() -> void override;

		auto SetChild(const TSharedPtr<MWidget>& InChild) -> void { ChildWidget = InChild; }

		auto GetChild() const -> TSharedPtr<MWidget> { return ChildWidget; }

	protected:
		TSharedPtr<MWidget> ChildWidget;
	};
}