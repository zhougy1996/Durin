#include "Window/GenericWindow.h"

FGenericWindow::FGenericWindow() = default;

FGenericWindow::~FGenericWindow() = default;

auto FGenericWindow::Initialize(FGenericApplication* const InApplication, const TSharedPtr<FGenericWindowDefinition>& InDefinition) -> void
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

auto FGenericWindow::GetOSWindowHandle() const -> void*
{
	return nullptr;
}

auto FGenericWindow::ShouldClose() const -> bool
{
	return false;
}

auto FGenericWindow::Close() -> void
{
}
