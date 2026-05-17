#include "VulkanShader.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"

namespace Durin::VulkanRHI
{
	FVulkanShader::FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc)
		: FRHIShader(InCreateDesc)
		, Device(InDevice)
		, EntryPoint(InCreateDesc.EntryPoint)
	{
		vk::ShaderModuleCreateInfo createInfo;
		createInfo.setCode(InCreateDesc.Code);

		ShaderModule = Device.GetHandle().createShaderModule(createInfo);
	}

	FVulkanShader::~FVulkanShader()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::ShaderModule, ShaderModule);
	}

	auto FVulkanDynamicRHI::RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef
	{
		return new FVulkanShader(*Device, InCreateDesc);
	}
}