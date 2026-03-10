#pragma once

#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	public:
		auto Draw() -> void override;

		auto SetChild(TSharedPtr<MWidget> InChild) -> void { ChildWidget = std::move(InChild); }

		auto GetChild() const -> TSharedPtr<MWidget> { return ChildWidget; }

	protected:
		TSharedPtr<MWidget> ChildWidget;
	};
}