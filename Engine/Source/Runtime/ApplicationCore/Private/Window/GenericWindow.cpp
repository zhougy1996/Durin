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

	auto FGenericWindow::SetCursor(EMouseCursor Cursor) -> void
	{
	}

} // namespace Durin