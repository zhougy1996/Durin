#include "Widgets/MWindow.h"

#include <utility>

#include "Application/MonaApplication.h"

namespace Doge::Mona
{
	MWindow::~MWindow()
	{
		NativeWindow = nullptr;
	}

	auto MWindow::PollEvents() const -> void
	{
		NativeWindow->PollEvents();
	}

	auto MWindow::SetNativeWindow(TSharedPtr<FGenericWindow> InNativeWindow) -> void
	{
		NativeWindow = std::move(InNativeWindow);
		//TODO: set cached screen position and size when creating native window, currently we just set them to zero
		SetCachedSize({});
	}

	auto MWindow::GetNativeWindow() const -> TSharedPtr<FGenericWindow>
	{
		return NativeWindow;
	}

	auto MWindow::GetChildWindows() const -> const std::vector<TSharedPtr<MWindow>>&
	{
		return ChildWindows;
	}

	auto MWindow::RequestDestroyWindow() -> void
	{
		FMonaApplication::Get().RequestDestroyWindow(SharedThis(this));
	}

	auto MWindow::MoveWindowTo(const FVector2f& NewScreenPosition) -> void
	{
		if (NativeWindow != nullptr)
		{
			FVector2f NewPositionTruncated = FVector2f(FMath::TruncToFloat(NewScreenPosition.x), FMath::TruncToFloat(NewScreenPosition.y));
			SetCachedScreenPosition(NewPositionTruncated);

			NativeWindow->MoveWindowTo(FMath::TruncToInt(NewScreenPosition.x), FMath::TruncToInt(NewScreenPosition.y));
		}
		else
		{
			InitialDesiredScreenPosition = NewScreenPosition;
		}
	}

	auto MWindow::ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void
	{
		// TODO: return when window shape not changed
		if (NativeWindow != nullptr)
		{
			FVector2i NewPositionTruncated = FVector2i(FMath::TruncToInt(NewScreenPosition.x), FMath::TruncToInt(NewScreenPosition.y));
			FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));
			SetCachedScreenPosition(NewScreenPosition);

			NativeWindow->ReshapeWindow(NewPositionTruncated.x, NewPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);

			SetCachedSize(NewSize);
			ScreenPosition = NewScreenPosition;
		}
		else
		{
			InitialDesiredScreenPosition = NewScreenPosition;
			InitialDesiredSize = NewSize;
		}
	}

	auto MWindow::ResizeWindow(const FVector2f& NewSize) -> void
	{
		if (NativeWindow == nullptr)
		{
			InitialDesiredSize = NewSize;
			return;
		}
		FVector2i CurrentPositionTruncated = FVector2i(FMath::TruncToInt(ScreenPosition.x), FMath::TruncToInt(ScreenPosition.y));
		FVector2i NewSizeTruncated = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));

		NativeWindow->ReshapeWindow(CurrentPositionTruncated.x, CurrentPositionTruncated.y, NewSizeTruncated.x, NewSizeTruncated.y);
		SetCachedSize(NewSize);
	}

	auto MWindow::GetViewportSize() const -> FVector2f
	{
		return ViewportSize;
	}

	auto MWindow::SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void
	{
		ScreenPosition = NewScreenPosition;
	}

	auto MWindow::SetCachedSize(const FVector2f& NewSize) -> void
	{
		Size = NewSize;
		ViewportSize = NativeWindow->GetViewportSize();
	}

	auto MWindow::SetViewport(const std::shared_ptr<IMonaViewport>& InViewport) -> void
	{
		Viewport = InViewport;
	}

	auto MWindow::GetViewport() const -> std::shared_ptr<IMonaViewport>
	{
		return Viewport.lock();
	}

	auto MWindow::GetWindowMode() const -> EWindowMode
	{
		return NativeWindow->GetWindowMode();
	}

	auto MWindow::ShowWindow() -> void
	{

	}

	auto MWindow::HideWindow() -> void
	{
	}
} // namespace Doge::Mona