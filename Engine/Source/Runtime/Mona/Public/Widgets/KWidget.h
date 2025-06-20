#pragma once

class KLEE_API KWidget : public TSharedFromThis<KWidget>
{
public:
	KWidget() = default;
	KWidget(TSharedPtr<KWidget> ParentWidget);

	virtual ~KWidget() = default;

	virtual auto DrawWidget() -> void = 0;

	virtual auto AsWidget() -> TSharedPtr<KWidget>;

	virtual auto IsWindow() -> bool { return false; }

	auto AssignParentWidget(TSharedPtr<KWidget> ParentWidget);

	auto GetParent() -> TSharedPtr<KWidget>;

private:
	TWeakPtr<KWidget> ParentWidget_;
};
