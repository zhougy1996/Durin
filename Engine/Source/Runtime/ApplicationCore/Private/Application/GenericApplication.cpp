#include "Application/GenericApplication.h"

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
}
