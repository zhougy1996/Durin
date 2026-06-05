#include "Widgets/MCompoundWidget.h"

namespace Durin
{
	void MCompoundWidget::Draw()
	{
		DrawChild();
	}

	auto MCompoundWidget::SetChild(const std::shared_ptr<MWidget>& InChild) -> void
	{
		SetContent(InChild);
	}

	auto MCompoundWidget::GetChild() const -> std::shared_ptr<MWidget>
	{
		return GetContent();
	}

	auto MCompoundWidget::SetContent(const std::shared_ptr<MWidget>& InContent) -> MCompoundWidget&
	{
		if (ChildWidget == InContent)
		{
			return *this;
		}

		if (ChildWidget != nullptr && ChildWidget->GetParent().get() == this)
		{
			ChildWidget->AssignParentWidget(nullptr);
		}

		ChildWidget = InContent;
		if (ChildWidget != nullptr)
		{
			ChildWidget->AssignParentWidget(SharedThis(this));
		}

		return *this;
	}

	auto MCompoundWidget::ClearContent() -> void
	{
		SetContent(nullptr);
	}

	auto MCompoundWidget::GetContent() const -> std::shared_ptr<MWidget>
	{
		return ChildWidget;
	}

	auto MCompoundWidget::DrawChild() -> void
	{
		if (ChildWidget != nullptr)
		{
			ChildWidget->Draw();
		}
	}
} // namespace Durin
