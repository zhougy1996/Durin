#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	MWidget::MWidget(const TSharedPtr<MWidget>& InParentWidget)
		: ParentWidget(InParentWidget)
	{
	}

	auto MWidget::Render() -> void
	{
		OnRender();
	}

	auto MWidget::AsWidget() -> TSharedPtr<MWidget>
	{
		return AsShared();
	}

	auto MWidget::AssignParentWidget(TSharedPtr<MWidget> InParentWidget)
	{
		ParentWidget = InParentWidget;
	}

	auto MWidget::GetParent() const -> TSharedPtr<MWidget>
	{
		return ParentWidget.lock();
	}
}
