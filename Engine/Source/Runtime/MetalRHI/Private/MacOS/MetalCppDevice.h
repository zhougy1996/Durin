#pragma once

#include "CoreMinimal.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	struct FMetalCppDeviceAndQueue
	{
		NS::SharedPtr<MTL::Device> Device;
		NS::SharedPtr<MTL::CommandQueue> Queue;
	};

	// Returns independent RAII owners; partially created candidates release on failure.
	auto CreateMetalCppDeviceAndQueue() -> FMetalCppDeviceAndQueue;
}
