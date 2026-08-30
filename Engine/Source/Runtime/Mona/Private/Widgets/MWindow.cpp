#include "Widgets/MWindow.h"

#include "Application/MonaApplication.h"

namespace Durin
{
	MWindow::~MWindow()
	{
		SetContent(nullptr);
		NativeWindow = nullptr;
	}

	auto MWindow::Draw() -> void
	{
		if (ContentWidget != nullptr)
		{
			ContentWidget->Draw();
		}
	}

	auto MWindow::SetContent(const std::shared_ptr<MWidget>& InContent) -> MWindow&
	{
		if (ContentWidget == InContent)
		{
			return *this;
		}

		if (ContentWidget != nullptr && ContentWidget->GetParent().get() == this)
		{
			ContentWidget->AssignParentWidget(nullptr);
		}

		ContentWidget = InContent;
		if (ContentWidget != nullptr)
		{
			ContentWidget->AssignParentWidget(SharedThis(this));
		}

		return *this;
	}

	auto MWindow::PollEvents() const -> void
	{
		NativeWindow->PollEvents();
	}

	auto MWindow::SetNativeWindow(std::shared_ptr<FGenericWindow> InNativeWindow) -> void
	{
		NativeWindow = std::move(InNativeWindow);
		NativeWindow->SetTitleBarDarkMode(bTitleBarDarkMode);
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
		Mona::FMonaApplication::Get().RequestDestroyWindow(SharedThis(this));
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

	auto MWindow::SetWindowDecorationMode(EWindowDecorationMode Mode) -> void
	{
		WindowDecorationMode = Mode;
		if (NativeWindow != nullptr)
		{
			NativeWindow->SetWindowDecorationMode(Mode);
		}
	}

	auto MWindow::GetWindowDecorationMode() const -> EWindowDecorationMode
	{
		return WindowDecorationMode;
	}

	auto MWindow::GetEffectiveWindowDecorationMode() const -> EWindowDecorationMode
	{
		return NativeWindow != nullptr ? NativeWindow->GetEffectiveWindowDecorationMode() : WindowDecorationMode;
	}

	auto MWindow::PublishTitleBarLayout(const FWindowTitleBarLayout& Layout) -> void
	{
		if (NativeWindow != nullptr) NativeWindow->PublishTitleBarLayout(Layout);
	}

	auto MWindow::GetTitleBarInteractionState() const -> FWindowTitleBarInteractionState
	{
		return NativeWindow != nullptr ? NativeWindow->GetTitleBarInteractionState() : FWindowTitleBarInteractionState{};
	}

	auto MWindow::GetTitleBarPlatformMetrics() const -> FWindowTitleBarPlatformMetrics
	{
		return NativeWindow != nullptr ? NativeWindow->GetTitleBarPlatformMetrics() : FWindowTitleBarPlatformMetrics{};
	}

	auto MWindow::SetTitleBarDarkMode(bool bDarkMode) -> void
	{
		bTitleBarDarkMode = bDarkMode;
		if (NativeWindow != nullptr)
		{
			NativeWindow->SetTitleBarDarkMode(bDarkMode);
		}
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
		FVector2i NewIntSize = FVector2i(FMath::TruncToInt(NewSize.x), FMath::TruncToInt(NewSize.y));

		if (NativeWindow)
		{
			NativeWindow->ResizeWindow(NewIntSize.x, NewIntSize.y);
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
	}

	auto MWindow::SetCachedViewportSize(const FVector2f& NewViewportSize) -> void
	{
		ViewportSize = NewViewportSize;
	}

	auto MWindow::GetWindowMode() const -> EWindowMode
	{
		return NativeWindow->GetWindowMode();
	}

	auto MWindow::SetViewportPresentationPolicy(EViewportPresentationPolicy InPolicy) -> void
	{
		ViewportPresentationPolicy = InPolicy;
	}

	auto MWindow::GetViewportPresentationPolicy() const -> EViewportPresentationPolicy
	{
		return ViewportPresentationPolicy;
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

	auto MWindow::IsMaximized() const -> bool
	{
		return NativeWindow ? NativeWindow->IsMaximized() : false;
	}

	auto MWindow::MaximizeWindow() -> void
	{
		if (NativeWindow != nullptr)
		{
			NativeWindow->MaximizeWindow();
		}
	}

	auto MWindow::RestoreWindow() -> void
	{
		if (NativeWindow != nullptr)
		{
			NativeWindow->RestoreWindow();
		}
	}

	auto MWindow::MinimizeWindow() -> void
	{
		if (NativeWindow != nullptr) NativeWindow->MinimizeWindow();
	}
} // namespace Durin
