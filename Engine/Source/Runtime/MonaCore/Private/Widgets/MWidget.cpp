#include "Widgets/MWidget.h"

namespace Durin::Mona
{
	MWidget::MWidget(const std::shared_ptr<MWidget>& InParentWidget)
		: ParentWidget(InParentWidget)
	{
	}

	auto MWidget::Construct() -> void
	{
	}

	auto MWidget::Draw() -> void
	{
	}

	auto MWidget::AsWidget() -> std::shared_ptr<MWidget>
	{
		return shared_from_this();
	}

	auto MWidget::AssignParentWidget(const std::shared_ptr<MWidget>& InParentWidget) -> void
	{
		ParentWidget = InParentWidget;
	}

	auto MWidget::GetParent() const -> std::shared_ptr<MWidget>
	{
		return ParentWidget.lock();
	}
}
