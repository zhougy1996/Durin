#pragma once

#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	public:
		auto SetChild(TSharedPtr<MWidget> InChild) -> void { ChildWidget = std::move(InChild); }

		auto GetChild() const -> TSharedPtr<MWidget> { return ChildWidget; }
	protected:
		auto OnRender() -> void override;

		TSharedPtr<MWidget> ChildWidget;
	};
}