#pragma once

#include "MonaCoreAPI.h"
#include "Widgets/MWidget.h"

namespace Durin::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	public:
		auto Draw() -> void override;

		auto SetChild(const std::shared_ptr<MWidget>& InChild) -> void { ChildWidget = InChild; }

		auto GetChild() const -> std::shared_ptr<MWidget> { return ChildWidget; }

	protected:
		std::shared_ptr<MWidget> ChildWidget;
	};
}