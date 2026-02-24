#pragma once

#include "Window/GenericWindow.h"
#include "Widgets/MWidget.h"

class FRHIViewport;
class FGenericWindow;

class MONACORE_API MWindow : public MWidget
{
public:
	virtual ~MWindow();

	virtual auto DrawWidget() -> void override;

	auto PollEvents() const -> void;

	auto SetNativeWindow(TSharedPtr<FGenericWindow> InNativeWindow) -> void;

	auto GetNativeWindow() const -> TSharedPtr<FGenericWindow>;

	auto GetChildWindows() const -> const std::vector<TSharedPtr<MWindow>>&;

	auto RequestDestroyWindow() -> void;

	auto GetTitle() const -> FString { return Title_; }

	auto SetTitle(const FString& InTitle) -> void { Title_ = InTitle; }

	auto GetDesiredScreenPosition() const -> FVector2f { return InitialDesiredScreenPosition_; }

	auto GetDesiredSize() const -> FVector2f { return InitialDesiredSize_; }

	auto MoveWindowTo(const FVector2f& NewScreenPosition) -> void;

	auto ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void;

	auto ResizeWindow(const FVector2f& NewSize) -> void;

	auto GetScreenPosition() const -> FVector2f { return ScreenPosition_; }

	auto GetWindowSize() const -> FVector2f { return Size_; }

	auto GetViewportSize() const -> FVector2f;

	auto SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void;

	auto SetCachedSize(const FVector2f& NewSize) -> void;

	auto SetRHIViewport(TSharedPtr<FRHIViewport> RHIViewport) -> void;

	auto GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&;

	auto GetWindowMode() const -> EWindowMode;

	auto IsWindow() -> bool override { return true; }

protected:
	FString Title_;

	FVector2f InitialDesiredScreenPosition_;

	FVector2f InitialDesiredSize_;

	FVector2f ScreenPosition_;

	FVector2f Size_;

	FVector2f ViewportSize_;

	TSharedPtr<FGenericWindow> NativeWindow_;

	std::vector<TSharedPtr<MWindow>> ChildWindows_;

	TSharedPtr<FRHIViewport> RHIViewport_;
};
