#include "Application/GenericApplication.h"

auto FGenericApplication::Tick() -> void
{
}

auto FGenericApplication::ProcessDeferredEvents() -> void
{
}
auto FGenericApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow>
{
	return nullptr;
}
