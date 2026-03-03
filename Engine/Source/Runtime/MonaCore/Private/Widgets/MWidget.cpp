#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	MWidget::MWidget(TSharedPtr<MWidget> InParentWidget)
		: ParentWidget(std::move(InParentWidget))
	{
	}

	auto MWidget::Paint() -> void
	{
		OnPaint();
	}

	auto MWidget::AsWidget() -> TSharedPtr<MWidget>
	{
		return AsShared();
	}

	auto MWidget::AssignParentWidget(TSharedPtr<MWidget> InParentWidget)
	{
		ParentWidget = std::move(InParentWidget);
	}

	auto MWidget::GetParent() const -> TSharedPtr<MWidget>
	{
		return ParentWidget.lock();
	}
}
