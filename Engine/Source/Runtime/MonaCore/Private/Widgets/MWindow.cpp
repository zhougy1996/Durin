#include "Widgets/MWindow.h"

#include <utility>

#include "Application/MonaApplication.h"

namespace Doge::Mona
{
	MWindow::~MWindow()
	{
		NativeWindow_ = nullptr;
	}

	auto MWindow::DrawWidget() -> void
	{
	}

	auto MWindow::PollEvents() const -> void
	{
		NativeWindow_->PollEvents();
	}

	auto MWindow::SetNativeWindow(TSharedPtr<FGenericWindow> InNativeWindow) -> void
	{
		NativeWindow_ = std::move(InNativeWindow);
		//TODO: set cached screen position and size when creating native window, currently we just set them to zero
		SetCachedSize({});
	}

	auto MWindow::GetNativeWindow() const -> TSharedPtr<FGenericWindow>
	{
		return NativeWindow_;
	}

	auto MWindow::GetChildWindows() const -> const std::vector<TSharedPtr<MWindow>>&
	{
		return ChildWindows_;
	}

	auto MWindow::RequestDestroyWindow() -> void
	{
		FMonaApplication::Get().RequestDestroyWindow(SharedThis(this));
	}

	auto MWindow::MoveWindowTo(const FVector2f& NewScreenPosition) -> void
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

	auto MWindow::ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void
	{
		// TODO: return when window shape not changed
		if (NativeWindow_ != nullptr)
		{
			FVector2i NewPositionTruncated = FVector2i(FMath::TruncToInt(NewScreenPosition.x), FMath::TruncToInt(NewScreenPosition.y));
			FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));
			SetCachedScreenPosition(NewScreenPosition);

			NativeWindow_->ReshapeWindow(NewPositionTruncated.x, NewPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);

			SetCachedSize(NewSize);
			ScreenPosition_ = NewScreenPosition;
		}
		else
		{
			InitialDesiredScreenPosition_ = NewScreenPosition;
			InitialDesiredSize_ = NewSize;
		}
	}

	auto MWindow::ResizeWindow(const FVector2f& NewSize) -> void
	{
		if (NativeWindow_ == nullptr)
		{
			InitialDesiredSize_ = NewSize;
			return;
		}
		FVector2i CurrentPositionTruncated = FVector2i(FMath::TruncToInt(ScreenPosition_.x), FMath::TruncToInt(ScreenPosition_.y));
		FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));

		NativeWindow_->ReshapeWindow(CurrentPositionTruncated.x, CurrentPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);
		SetCachedSize(NewSize);
	}

	auto MWindow::GetViewportSize() const -> FVector2f
	{
		return ViewportSize_;
	}

	auto MWindow::SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void
	{
		ScreenPosition_ = NewScreenPosition;
	}

	auto MWindow::SetCachedSize(const FVector2f& NewSize) -> void
	{
		Size_ = NewSize;
		ViewportSize_ = NativeWindow_->GetViewportSize();
	}

	auto MWindow::SetRHIViewport(TSharedPtr<FRHIViewport> RHIViewport) -> void
	{
		RHIViewport_ = std::move(RHIViewport);
	}

	auto MWindow::GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&
	{
		return RHIViewport_;
	}

	auto MWindow::GetWindowMode() const -> EWindowMode
	{
		return NativeWindow_->GetWindowMode();
	}
}