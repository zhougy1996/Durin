#include "Window/GenericWindow.h"

namespace Durin
{
	auto HitTestWindowTitleBar(
		const FWindowTitleBarLayout& Layout,
		FIntPoint Point,
		FIntPoint ClientSize,
		int32 ResizeBorderX,
		int32 ResizeBorderY,
		bool bAllowResize) -> EWindowTitleBarHitTest
	{
		if (bAllowResize)
		{
			const bool bLeft = Point.x >= 0 && Point.x < ResizeBorderX;
			const bool bRight = Point.x >= ClientSize.x - ResizeBorderX && Point.x < ClientSize.x;
			const bool bTop = Point.y >= 0 && Point.y < ResizeBorderY;
			const bool bBottom = Point.y >= ClientSize.y - ResizeBorderY && Point.y < ClientSize.y;
			if (bTop && bLeft) return EWindowTitleBarHitTest::ResizeTopLeft;
			if (bTop && bRight) return EWindowTitleBarHitTest::ResizeTopRight;
			if (bBottom && bLeft) return EWindowTitleBarHitTest::ResizeBottomLeft;
			if (bBottom && bRight) return EWindowTitleBarHitTest::ResizeBottomRight;
			if (bLeft) return EWindowTitleBarHitTest::ResizeLeft;
			if (bRight) return EWindowTitleBarHitTest::ResizeRight;
			if (bTop) return EWindowTitleBarHitTest::ResizeTop;
			if (bBottom) return EWindowTitleBarHitTest::ResizeBottom;
		}

		if (!Layout.bValid) return EWindowTitleBarHitTest::Client;
		if (Layout.CloseRegion.Contains(Point)) return EWindowTitleBarHitTest::Close;
		if (Layout.MaximizeRegion.Contains(Point)) return EWindowTitleBarHitTest::Maximize;
		if (Layout.MinimizeRegion.Contains(Point)) return EWindowTitleBarHitTest::Minimize;
		for (const FWindowTitleBarRect& Region : Layout.DragRegions)
		{
			if (Region.Contains(Point)) return EWindowTitleBarHitTest::Caption;
		}
		return EWindowTitleBarHitTest::Client;
	}

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

	auto FGenericWindow::MinimizeWindow() -> void
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

	auto FGenericWindow::SetWindowDecorationMode(EWindowDecorationMode Mode) -> void
	{
		if (Definition != nullptr) Definition->DecorationMode = Mode;
	}

	auto FGenericWindow::GetWindowDecorationMode() const -> EWindowDecorationMode
	{
		return Definition != nullptr ? Definition->DecorationMode : EWindowDecorationMode::System;
	}

	auto FGenericWindow::GetEffectiveWindowDecorationMode() const -> EWindowDecorationMode
	{
		return EffectiveDecorationMode;
	}

	auto FGenericWindow::PublishTitleBarLayout(const FWindowTitleBarLayout& Layout) -> void
	{
		(void)Layout;
	}

	auto FGenericWindow::GetTitleBarInteractionState() const -> FWindowTitleBarInteractionState
	{
		return {};
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
