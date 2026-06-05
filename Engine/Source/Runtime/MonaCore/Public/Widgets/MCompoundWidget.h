#pragma once

#include "MonaCoreAPI.h"
#include "Widgets/MWidget.h"

namespace Durin::Mona
{
	class MONACORE_API MCompoundWidget : public MWidget
	{
	public:
		auto Draw() -> void override;

		auto SetChild(const std::shared_ptr<MWidget>& InChild) -> void;

		auto GetChild() const -> std::shared_ptr<MWidget>;

		auto SetContent(const std::shared_ptr<MWidget>& InContent) -> MCompoundWidget&;

		auto ClearContent() -> void;

		auto GetContent() const -> std::shared_ptr<MWidget>;

	protected:
		auto DrawChild() -> void;

	protected:
		std::shared_ptr<MWidget> ChildWidget;
	};
}
