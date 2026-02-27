#pragma once

namespace Doge::Mona
{
	class MONACORE_API MWidget : public TSharedFromThis<MWidget>
	{
	public:
		MWidget() = default;
		MWidget(TSharedPtr<MWidget> ParentWidget);

		virtual ~MWidget() = default;

		virtual auto DrawWidget() -> void = 0;

		virtual auto AsWidget() -> TSharedPtr<MWidget>;

		virtual auto IsWindow() -> bool { return false; }

		auto AssignParentWidget(TSharedPtr<MWidget> ParentWidget);

		auto GetParent() -> TSharedPtr<MWidget>;

	private:
		TWeakPtr<MWidget> ParentWidget_;
	};
}