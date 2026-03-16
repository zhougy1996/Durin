#pragma once

namespace Doge::Mona
{
	class MONACORE_API MWidget : public std::enable_shared_from_this<MWidget>
	{
	public:
		MWidget() = default;
		MWidget(const std::shared_ptr<MWidget>& InParentWidget);

		virtual ~MWidget() = default;

		virtual auto Draw() -> void;

		virtual auto AsWidget() -> std::shared_ptr<MWidget>;

		virtual auto IsWindow() -> bool { return false; }

		auto AssignParentWidget(const std::shared_ptr<MWidget>& InParentWidget);

		auto GetParent() const -> std::shared_ptr<MWidget>;

		auto SetName(FName InName) -> MWidget* { Name = InName; return this; }

		auto GetName() const -> FName { return Name; }

	protected:
		FName Name;

		std::weak_ptr<MWidget> ParentWidget;
	};
}