#pragma once

#include "MonaCoreAPI.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class MCompoundWidget : public MWidget
	{
	public:
		MONACORE_API auto Draw() -> void override;

		MONACORE_API auto SetChild(const std::shared_ptr<MWidget>& InChild) -> void;

		MONACORE_API auto GetChild() const -> std::shared_ptr<MWidget>;

		MONACORE_API auto SetContent(const std::shared_ptr<MWidget>& InContent) -> MCompoundWidget&;

		MONACORE_API auto ClearContent() -> void;

		MONACORE_API auto GetContent() const -> std::shared_ptr<MWidget>;

	protected:
		MONACORE_API auto DrawChild() -> void;

		std::shared_ptr<MWidget> ChildWidget;
	};
}
