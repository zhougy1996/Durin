#include "Widgets/MWindow.h"

#include <utility>

#include "Application/MonaApplication.h"
#include "Window/GlfwWindow.h"

namespace Durin::Mona
{
	MWindow::~MWindow()
	{
		NativeWindow = nullptr;
	}

	auto MWindow::PollEvents() const -> void
	{
		NativeWindow->PollEvents();
	}

	auto MWindow::SetNativeWindow(std::shared_ptr<FGenericWindow> InNativeWindow) -> void
	{
		NativeWindow = std::move(InNativeWindow);
		ViewportSize = NativeWindow->GetViewportSize();
	}

	auto MWindow::GetNativeWindow() const -> std::shared_ptr<FGenericWindow>
	{
		return NativeWindow;
	}

	auto MWindow::GetChildWindows() const -> const std::vector<std::shared_ptr<MWindow>>&
	{
		return ChildWindows;
	}

	auto MWindow::RequestDestroyWindow() -> void
	{
		FMonaApplication::Get().RequestDestroyWindow(SharedThis(this));
	}

	auto MWindow::GetTitle() const -> std::string
	{
		return Title;
	}

	auto MWindow::SetTitle(const std::string& InTitle) -> void
	{
		Title = InTitle;
	}

	auto MWindow::GetDesiredScreenPosition() const -> FVector2f
	{
		return InitialDesiredScreenPosition;
	}

	auto MWindow::GetDesiredSize() const -> FVector2f
	{
		return InitialDesiredSize;
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
		FVector2i NewIntScreenPosition = FVector2i(FMath::TruncToInt(ScreenPosition.x), FMath::TruncToInt(ScreenPosition.y));
		FVector2i NewIntSize = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));

		if (NativeWindow)
		{
			NativeWindow->ReshapeWindow(NewIntScreenPosition.x, NewIntScreenPosition.y, NewIntSize.x, NewIntSize.y);
		}
		else
		{
			InitialDesiredSize = NewSize;
		}

		SetCachedSize(NewSize);
	}

	auto MWindow::GetScreenPosition() const -> FVector2f
	{
		return ScreenPosition;
	}

	auto MWindow::GetWindowSize() const -> FVector2f
	{
		return Size;
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
		if (NativeWindow)
		{
			ViewportSize = NativeWindow->GetViewportSize();
		}
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

	bool MWindow::IsWindow()
	{
		return true;
	}

	auto MWindow::ShowWindow() -> void
	{
	}

	auto MWindow::HideWindow() -> void
	{
	}

	auto MWindow::IsMinimized() const -> bool
	{
		return NativeWindow->IsMinimized();
	}
} // namespace Durin::Mona