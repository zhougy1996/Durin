#include "VulkanShader.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanShader::FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc)
		: FRHIShader(InCreateDesc)
		, Device(InDevice)
		, EntryPoint(InCreateDesc.EntryPoint)
	{
		CheckVulkanRHIThread();
		vk::ShaderModuleCreateInfo createInfo;
		// The shader code is expected to be in SPIR-V bytecode format, which is a binary format where each instruction is 4 bytes (32 bits) long. Therefore, the size of the code should be a multiple of 4 bytes.
		check(InCreateDesc.Code.size() % sizeof(uint32) == 0);
		std::span Code = {
			reinterpret_cast<const uint32*>(InCreateDesc.Code.data()),
			InCreateDesc.Code.size() / sizeof(uint32)
		};
		createInfo.setCode(Code);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::ShaderModule);
#endif
		ShaderModule = Device.GetHandle().createShaderModule(createInfo);
		Device.GetRHI().GetDebugUtils().NameObject(ShaderModule,
			InCreateDesc.DebugName ? InCreateDesc.DebugName
				: Device.GetRHI().GetDebugUtils().MakeInternalName("ShaderModule"));
	}

	FVulkanShader::~FVulkanShader()
	{
		CheckVulkanRHIThread();
		if (ShaderModule)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::ShaderModule, ShaderModule);
		}
	}

	auto FVulkanDynamicRHI::RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef
	{
		FShaderRHIRef Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, InCreateDesc, &Result]() {
					Result = new FVulkanShader(*Device, InCreateDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI shader '{}': {}",
				InCreateDesc.DebugName ? InCreateDesc.DebugName : "<unnamed>",
				CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}
}
