#include "Widgets/KWidget.h"

KWidget::KWidget(TSharedPtr<KWidget> ParentWidget)
	: ParentWidget_(ParentWidget)
{
}

auto KWidget::AsWidget() -> TSharedPtr<KWidget>
{
	return AsShared();
}

auto KWidget::AssignParentWidget(TSharedPtr<KWidget> ParentWidget)
{
	ParentWidget_ = ParentWidget;
}

auto KWidget::GetParent() -> TSharedPtr<KWidget>
{
	return ParentWidget_.lock();
}
