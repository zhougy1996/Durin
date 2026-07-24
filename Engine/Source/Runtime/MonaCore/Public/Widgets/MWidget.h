#pragma once

#include "MonaCoreAPI.h"

namespace Durin
{
	// Provides the shared ownership, hierarchy, and draw contract for Mona widgets.
	class MWidget : public std::enable_shared_from_this<MWidget>
	{
	public:
		MWidget() = default;

		MONACORE_API MWidget(const std::shared_ptr<MWidget>& InParentWidget);

		MONACORE_API virtual ~MWidget() = default;

		MONACORE_API virtual auto Construct() -> void;

		// Immediate UI paint hook used by the active Mona UI backend.
		MONACORE_API virtual auto Draw() -> void;

		MONACORE_API virtual auto AsWidget() -> std::shared_ptr<MWidget>;

		virtual auto IsWindow() -> bool { return false; }

		MONACORE_API auto AssignParentWidget(const std::shared_ptr<MWidget>& InParentWidget) -> void;

		MONACORE_API auto GetParent() const -> std::shared_ptr<MWidget>;

		auto SetName(FName InName) -> MWidget* { Name = InName; return this; }

		auto GetName() const -> FName { return Name; }

	protected:
		FName Name;

		// Weak ownership prevents parent-child widget cycles.
		std::weak_ptr<MWidget> ParentWidget;
	};
}
