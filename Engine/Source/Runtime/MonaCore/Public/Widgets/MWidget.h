#pragma once

namespace Doge::Mona
{
	class MONACORE_API MWidget : public TSharedFromThis<MWidget>
	{
	public:
		MWidget() = default;
		MWidget(TSharedPtr<MWidget> InParentWidget);

		virtual ~MWidget() = default;

		auto Render() -> void;

		virtual auto AsWidget() -> TSharedPtr<MWidget>;

		virtual auto IsWindow() -> bool { return false; }

		auto AssignParentWidget(TSharedPtr<MWidget> InParentWidget);

		auto GetParent() const -> TSharedPtr<MWidget>;

		auto SetName(FName InName) -> MWidget* { Name = InName; return this; }

		auto GetName() const -> FName { return Name; }

	protected:
		virtual auto OnRender() -> void {};

		FName Name;

		TWeakPtr<MWidget> ParentWidget;
	};
}