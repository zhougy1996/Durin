#pragma once

namespace Doge::Mona
{
	class MONACORE_API MWidget : public TSharedFromThis<MWidget>
	{
	public:
		MWidget() = default;
		MWidget(const TSharedPtr<MWidget>& InParentWidget);

		virtual ~MWidget() = default;

		auto Render() -> void;

		virtual auto AsWidget() -> TSharedPtr<MWidget>;

		virtual auto IsWindow() -> bool { return false; }

		auto AssignParentWidget(TSharedPtr<MWidget> ParentWidget);

		auto GetParent() const -> TSharedPtr<MWidget>;

	protected:
		virtual auto OnRender() -> void {};

		TWeakPtr<MWidget> ParentWidget;
	};
}