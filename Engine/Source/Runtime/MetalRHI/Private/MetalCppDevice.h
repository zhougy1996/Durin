#pragma once

#include <Metal/Metal.hpp>

namespace Durin
{
	struct FMetalCppDeviceAndQueue
	{
		MTL::Device* Device = nullptr;
		MTL::CommandQueue* Queue = nullptr;
	};

	// Returns retained native objects. The Objective-C++ caller transfers both
	// references to ARC before using them.
	auto CreateMetalCppDeviceAndQueue() -> FMetalCppDeviceAndQueue;
}
