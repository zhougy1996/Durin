#include "VulkanShader.h"

#include <iostream>
#include <fstream>

#include "VulkanDevice.h"

namespace Doge::VulkanRHI
{
	static std::vector<char> ReadShaderFile(const std::string& filename)
	{
		std::ifstream File(filename, std::ios::ate | std::ios::binary);

		if (!File.is_open())
		{
			throw std::runtime_error("failed to open file!");
		}

		size_t FileSize = (size_t)File.tellg();
		std::vector<char> Buffer(FileSize);

		File.seekg(0);
		File.read(Buffer.data(), FileSize);
		File.close();

		return Buffer;
	}

	FVulkanShader::FVulkanShader(FVulkanDevice& InDevice, const std::string& Filename, vk::ShaderStageFlagBits InStage)
		: Device(InDevice)
	{
		std::vector<char> ShaderCode = ReadShaderFile(Filename);

		vk::ShaderModuleCreateInfo createInfo;
		createInfo.setCodeSize(ShaderCode.size());
		createInfo.setPCode(reinterpret_cast<const uint32_t*>(ShaderCode.data()));

		try
		{
			ShaderModule = Device.GetHandle().createShaderModule(createInfo);
		}
		catch (const std::runtime_error& err)
		{
			DOGE_ERROR("Failed to create shader {}: {}", Filename, err.what());
		}
	}

	FVulkanShader::~FVulkanShader()
	{
		Device.GetHandle().destroyShaderModule(ShaderModule);
	}
}