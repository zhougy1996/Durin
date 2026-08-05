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
		ShaderModule = Device.GetHandle().createShaderModule(createInfo);
	}

	FVulkanShader::~FVulkanShader()
	{
		CheckVulkanRHIThread();
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ShaderModule, ShaderModule);
	}

	auto FVulkanDynamicRHI::RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef
	{
		FShaderRHIRef Result;
		if (GRHIThread && !IsInRHIThread())
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false,
				[this, InCreateDesc, &Result]() {
					Result = new FVulkanShader(*Device, InCreateDesc);
				});
			return Result;
		}
		CheckVulkanRHIThread();
		return new FVulkanShader(*Device, InCreateDesc);
	}
}
