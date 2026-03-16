#include "Application/GenericApplication.h"

namespace Doge
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
}
