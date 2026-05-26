#include "Application/GenericApplication.h"

#include "Window/GlfwWindow.h"

namespace Durin
{
	auto FGenericApplication::Tick() -> void
	{
	}

	auto FGenericApplication::ProcessDeferredEvents() -> void
	{
	}
	auto FGenericApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow>
	{
		return nullptr;
	}

	auto MakePlatformWindow() -> std::shared_ptr<FGenericWindow>
	{
		return FGlfwWindow::Make();
	}
} // namespace Durin
