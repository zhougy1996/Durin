#include "Widgets/MWidget.h"

namespace Doge
{
	MWidget::MWidget(TSharedPtr<MWidget> ParentWidget)
		: ParentWidget_(ParentWidget)
	{
	}

	auto MWidget::AsWidget() -> TSharedPtr<MWidget>
	{
		return AsShared();
	}

	auto MWidget::AssignParentWidget(TSharedPtr<MWidget> ParentWidget)
	{
		ParentWidget_ = ParentWidget;
	}

	auto MWidget::GetParent() -> TSharedPtr<MWidget>
	{
		return ParentWidget_.lock();
	}
}
