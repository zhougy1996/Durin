#include "Widgets/KWindow.h"

#include "Application/MonaApplication.h"

KWindow::~KWindow()
{
	NativeWindow_ = nullptr;
}

auto KWindow::DrawWidget() -> void
{
}

auto KWindow::PollEvents() const -> void
{
	NativeWindow_->PollEvents();
}

auto KWindow::SetNativeWindow(TSharedPtr<FGenericWindow> InNativeWindow) -> void
{
	NativeWindow_ = InNativeWindow;
}

auto KWindow::GetNativeWindow() const -> TSharedPtr<FGenericWindow>
{
	return NativeWindow_;
}

auto KWindow::GetChildWindows() const -> const TArray<TSharedPtr<KWindow>>&
{
	return ChildWindows_;
}

auto KWindow::RequestDestroyWindow() -> void
{
	FKleeApplication::Get().RequestDestroyWindow(SharedThis(this));
}

auto KWindow::MoveWindowTo(const FVector2f& NewScreenPosition) -> void
{
	if (NativeWindow_ != nullptr)
	{
		FVector2f NewPositionTruncated = FVector2f(FMath::TruncToFloat(NewScreenPosition.x), FMath::TruncToFloat(NewScreenPosition.y));
		SetCachedScreenPosition(NewPositionTruncated);

		NativeWindow_->MoveWindowTo(FMath::TruncToInt(NewScreenPosition.x), FMath::TruncToInt(NewScreenPosition.y));
	}
	else
	{
		InitialDesiredScreenPosition_ = NewScreenPosition;
	}
}

auto KWindow::ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void
{
	// TODO: return when window shape not changed
	if (NativeWindow_ != nullptr)
	{
		FVector2i NewPositionTruncated = FVector2i(FMath::TruncToInt(NewScreenPosition.x), FMath::TruncToInt(NewScreenPosition.y));
		FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));
		SetCachedScreenPosition(NewScreenPosition);
		SetCachedSize(NewSize);

		NativeWindow_->ReshapeWindow(NewPositionTruncated.x, NewPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);
		ScreenPosition_ = NewScreenPosition;
	}
	else
	{
		InitialDesiredScreenPosition_ = NewScreenPosition;
		InitialDesiredSize_ = NewSize;
	}
}

auto KWindow::ResizeWindow(const FVector2f& NewSize) -> void
{
	if (NativeWindow_ == nullptr)
	{
		InitialDesiredSize_ = NewSize;
		return;
	}
	FVector2i CurrentPositionTruncated = FVector2i(FMath::TruncToInt(ScreenPosition_.x), FMath::TruncToInt(ScreenPosition_.y));
	FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));

	SetCachedSize(NewSize);
	NativeWindow_->ReshapeWindow(CurrentPositionTruncated.x, CurrentPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);
}

auto KWindow::GetViewportSize() const -> FVector2f
{
	// TODO: Independent viewport size
	return Size_;
}

auto KWindow::SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void
{
	ScreenPosition_ = NewScreenPosition;
}

auto KWindow::SetCachedSize(const FVector2f& NewSize) -> void
{
	Size_ = NewSize;
}

auto KWindow::SetRHIViewport(TSharedPtr<FRHIViewport> RHIViewport) -> void
{
	RHIViewport_ = std::move(RHIViewport);
}

auto KWindow::GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&
{
	return RHIViewport_;
}

auto KWindow::GetWindowMode() const -> EWindowMode
{
	return NativeWindow_->GetWindowMode();
}
