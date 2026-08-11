#include "Window/GenericWindow.h"

namespace Durin
{
	FGenericWindow::FGenericWindow() = default;

	FGenericWindow::~FGenericWindow() = default;

	auto FGenericWindow::Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void
	{
	}

	auto FGenericWindow::PollEvents() const -> void
	{
	}

	auto FGenericWindow::WaitEventsTimeout(double TimeoutSeconds) const -> void
	{
		(void)TimeoutSeconds;
	}

	auto FGenericWindow::ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void
	{
	}

	auto FGenericWindow::ResizeWindow(int32 Width, int32 Height) -> void
	{
	}

	auto FGenericWindow::MoveWindowTo(int32 X, int32 Y) -> void
	{
	}

	auto FGenericWindow::GetWindowPosition() const -> FIntPoint
	{
		return {0, 0};
	}

	auto FGenericWindow::GetWindowSize() const -> FIntPoint
	{
		return {0, 0};
	}

	auto FGenericWindow::GetCursorPosition() const -> FVector2d
	{
		return {0.0, 0.0};
	}

	auto FGenericWindow::GetWindowMode() const -> EWindowMode
	{
		return EWindowMode::Windowed;
	}

	auto FGenericWindow::SetWindowMode(EWindowMode WindowMode) -> void
	{
	}

	auto FGenericWindow::GetOSNativeWindowHandle() const -> void*
	{
		return OSNativeWindowHandle;
	}

	auto FGenericWindow::ShouldClose() const -> bool
	{
		return false;
	}

	auto FGenericWindow::SetShouldClose(bool bShouldClose) -> void
	{
	}

	auto FGenericWindow::Close() -> void
	{
	}

	auto FGenericWindow::Show() -> void
	{
	}

	auto FGenericWindow::Hide() -> void
	{
	}

	auto FGenericWindow::IsVisible() const -> bool
	{
		return false;
	}

	auto FGenericWindow::Focus() -> void
	{
	}

	auto FGenericWindow::GetViewportSize() const -> FIntPoint
	{
		DURIN_ERROR("GetViewportSize() is not implemented for the current platform.");
		return {0, 0};
	}

	auto FGenericWindow::CreateVulkanSurface(void* InVulkanInstance) const -> void*
	{
		return nullptr;
	}

	auto FGenericWindow::IsMinimized() const -> bool
	{
		return false;
	}

	auto FGenericWindow::IsMaximized() const -> bool
	{
		return false;
	}

	auto FGenericWindow::MaximizeWindow() -> void
	{
	}

	auto FGenericWindow::RestoreWindow() -> void
	{
	}

	auto FGenericWindow::IsFocused() const -> bool
	{
		return false;
	}

	auto FGenericWindow::IsHovered() const -> bool
	{
		return false;
	}

	auto FGenericWindow::SetTitle(const std::string& InTitle) -> void
	{
	}

	auto FGenericWindow::SetOpacity(float InOpacity) -> void
	{
	}

	auto FGenericWindow::SetWindowDecorated(bool bDecorated) -> void
	{
	}

	auto FGenericWindow::SetTitleBarDarkMode(bool bDarkMode) -> void
	{
	}

	auto FGenericWindow::SetMousePassthrough(bool bPassthrough) -> void
	{
	}

	auto FGenericWindow::SetFocusOnShow(bool bFocusOnShow) -> void
	{
	}

	auto FGenericWindow::GetDpiScale() const -> float
	{
		return 1.0f;
	}

	auto FGenericWindow::SetCursor(EMouseCursor Cursor) -> void
	{
		if (Cursor != EMouseCursor::None) CursorShape = Cursor;
	}

	auto FGenericWindow::SetCursorMode(ECursorMode InCursorMode) -> void
	{
		if (CursorMode == InCursorMode) return;
		const bool bLeavingCapture = CursorMode == ECursorMode::Captured;
		if (InCursorMode == ECursorMode::Captured)
		{
			CursorPositionBeforeCapture = GetCursorPosition();
			bHasCursorPositionBeforeCapture = true;
		}
		ApplyCursorMode(InCursorMode);
		CursorMode = InCursorMode;
		if (InCursorMode == ECursorMode::Free && bHasCursorPositionBeforeCapture)
		{
			SetCursorPosition(CursorPositionBeforeCapture);
			bHasCursorPositionBeforeCapture = false;
		}
	}

	auto FGenericWindow::ApplyCursorMode(ECursorMode InCursorMode) -> void
	{
		(void)InCursorMode;
	}

	auto FGenericWindow::SetCursorPosition(FVector2d Position) -> void
	{
		(void)Position;
	}

} // namespace Durin
