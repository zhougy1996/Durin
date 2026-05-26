#include "Widgets/MWindow.h"

#include "Application/MonaApplication.h"

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

	auto MWindow::SetParentWindow(const std::shared_ptr<MWindow>& InParentWindow) -> void
	{
		const std::shared_ptr<MWindow> ThisWindow = SharedThis(this);
		const std::shared_ptr<MWindow> CurrentParentWindow = ParentWindow.lock();

		if (CurrentParentWindow == InParentWindow)
		{
			return;
		}

		if (CurrentParentWindow != nullptr)
		{
			std::erase(CurrentParentWindow->ChildWindows, ThisWindow);
		}

		ParentWindow.reset();

		if (InParentWindow != nullptr)
		{
			ParentWindow = InParentWindow;
			if (std::ranges::find(InParentWindow->ChildWindows, ThisWindow) == InParentWindow->ChildWindows.end())
			{
				InParentWindow->ChildWindows.push_back(ThisWindow);
			}
		}
	}

	auto MWindow::GetParentWindow() const -> std::shared_ptr<MWindow>
	{
		return ParentWindow.lock();
	}

	auto MWindow::AddChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void
	{
		if (InChildWindow == nullptr || InChildWindow.get() == this)
		{
			return;
		}

		InChildWindow->SetParentWindow(SharedThis(this));
	}

	auto MWindow::RemoveChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void
	{
		if (InChildWindow == nullptr)
		{
			return;
		}

		if (InChildWindow->GetParentWindow().get() == this)
		{
			InChildWindow->SetParentWindow(nullptr);
		}
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
		if (NativeWindow != nullptr)
		{
			NativeWindow->SetTitle(InTitle);
		}
	}

	auto MWindow::SetWindowDecorated(bool bDecorated) -> void
	{
		bWindowDecorated = bDecorated;
		if (NativeWindow != nullptr)
		{
			NativeWindow->SetWindowDecorated(bDecorated);
		}
	}

	auto MWindow::IsWindowDecorated() const -> bool
	{
		return bWindowDecorated;
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
		if (NativeWindow != nullptr)
		{
			NativeWindow->Show();
		}
	}

	auto MWindow::HideWindow() -> void
	{
		if (NativeWindow != nullptr)
		{
			NativeWindow->Hide();
		}
	}

	auto MWindow::IsMinimized() const -> bool
	{
		return NativeWindow->IsMinimized();
	}
} // namespace Durin::Mona
