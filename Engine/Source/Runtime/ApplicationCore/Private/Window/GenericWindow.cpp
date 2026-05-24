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

	auto FGenericWindow::ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void
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

	auto FGenericWindow::Close() -> void
	{
	}

	auto FGenericWindow::Show() -> void
	{
	}

	auto FGenericWindow::Hide() -> void
	{
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

	auto FGenericWindow::IsFocused() const -> bool
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
	}

} // namespace Durin
