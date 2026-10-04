#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "MetalCppDevice.h"

#include <stdexcept>

namespace Durin
{
	auto CreateMetalCppDeviceAndQueue() -> FMetalCppDeviceAndQueue
	{
		if (NS::ProcessInfo::processInfo()->operatingSystemVersion().majorVersion < 27)
			throw std::runtime_error("MetalRHI requires macOS 27 or newer.");
		NS::SharedPtr<MTL::Device> Device =
			NS::TransferPtr(MTL::CreateSystemDefaultDevice());
		if (!Device)
			throw std::runtime_error("MetalRHI could not create a Metal device.");
		if (!Device->supportsFamily(MTL::GPUFamilyApple9))
			throw std::runtime_error("MetalRHI requires Apple GPU Family 9 or newer.");
		NS::SharedPtr<MTL::CommandQueue> Queue =
			NS::TransferPtr(Device->newCommandQueue());
		if (!Queue)
			throw std::runtime_error("MetalRHI could not create its command queue.");
		const FMetalCppDeviceAndQueue Result{
			.Device = Device.get(), .Queue = Queue.get()};
		Device.detach();
		Queue.detach();
		return Result;
	}
}
