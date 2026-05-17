#include "VulkanExtensions.h"

#include "CoreGlobals.h"

#include "VulkanDevice.h"

#ifdef __APPLE__
	#define VKB_ENABLE_PORTABILITY
#endif

namespace Durin::VulkanRHI
{
	inline constexpr const char* DurinSupportedInstanceExtensionNames[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
	#ifdef VKB_ENABLE_PORTABILITY
		VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
		VK_MVK_MACOS_SURFACE_EXTENSION_NAME,
	#endif
	};

	inline constexpr const char* DurinSupportedDeviceExtensionNames[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
	#ifdef VKB_ENABLE_PORTABILITY
		"VK_KHR_portability_subset",
	#endif
	};

	template<typename ExtensionType>
	static auto FindExtension(const std::vector<std::unique_ptr<ExtensionType>>& InExtensions, const char* InExtensionName) -> int32
	{
		for (int32 Index = 0; Index < InExtensions.size(); ++Index)
		{
			if (strcmp(InExtensionName, InExtensions[Index]->GetExtensionName()) == 0)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	template<typename ExtensionType>
	static auto AddRequiredExtensions(std::vector<std::unique_ptr<ExtensionType>>& Extensions, const std::vector<const char*>& RequiredExtensionNames)
	{
		for (const char* ExtensionName : RequiredExtensionNames)
		{
			auto it = std::find_if(Extensions.begin(), Extensions.end(), [ExtensionName](std::unique_ptr<ExtensionType>& Extension) {
				return Extension->GetExtensionName() == ExtensionName;
			});

			if (it == Extensions.end())
			{
				Extensions.push_back(std::make_unique<ExtensionType>(ExtensionName));
			}
		}
	}

	auto FVulkanInstanceExtension::GetDurinSupportedInstanceExtensions() -> FVulkanInstanceExtensionArray
	{
		FVulkanInstanceExtensionArray OutDurinInstanceExtensions;

		for (const char* ExtensionName : DurinSupportedInstanceExtensionNames)
		{
			OutDurinInstanceExtensions.push_back(std::make_unique<FVulkanInstanceExtension>(ExtensionName));
		}
		AddRequiredExtensions(OutDurinInstanceExtensions, GMonaRequiredVulkanInstanceExtensions);

		const std::vector<vk::ExtensionProperties> DriverSupportedInstanceExtensions = vk::enumerateInstanceExtensionProperties();
		DURIN_TRACE("Found {} available instance extensions:", DriverSupportedInstanceExtensions.size());
		for (const vk::ExtensionProperties& Extension : DriverSupportedInstanceExtensions)
		{
			const int32 ExtensionIndex = FindExtension(OutDurinInstanceExtensions, Extension.extensionName);
			const bool bFound = (ExtensionIndex != INDEX_NONE);
			if (bFound)
			{
				// Set the extension as supported and activated temporarily.
				// TODO: some extensions may not be activated by default.
				OutDurinInstanceExtensions[ExtensionIndex]->SetSupported();
				OutDurinInstanceExtensions[ExtensionIndex]->SetActivated();
			}
			DURIN_TRACE("{} {}", bFound ? "+" : "-", Extension.extensionName.data());
		}

		return OutDurinInstanceExtensions;
	}

	auto FVulkanDeviceExtension::GetDurinSupportedDeviceExtensions(FVulkanDevice* InDevice) -> FVulkanDeviceExtensionArray
	{
		FVulkanDeviceExtensionArray OutDeviceExtensions;

		for (const char* ExtensionName : DurinSupportedDeviceExtensionNames)
		{
			OutDeviceExtensions.push_back(std::make_unique<FVulkanDeviceExtension>(ExtensionName));
		}

		std::vector<vk::ExtensionProperties> DriverSupportedDeviceExtensions = GetDriverSupportedDeviceExtensions(InDevice->GetGpu());
		DURIN_TRACE("Found {} available device extensions:", DriverSupportedDeviceExtensions.size());

		for (const vk::ExtensionProperties& Extension : DriverSupportedDeviceExtensions)
		{
			const int32 ExtensionIndex = FindExtension(OutDeviceExtensions, Extension.extensionName);
			const bool bFound = (ExtensionIndex != INDEX_NONE);
			if (bFound)
			{
				// Set the extension as supported and activated temporarily.
				// TODO: some extensions may not be activated by default.
				OutDeviceExtensions[ExtensionIndex]->SetSupported();
				OutDeviceExtensions[ExtensionIndex]->SetActivated();
			}
			DURIN_TRACE("{} {}", bFound ? "+" : "-", Extension.extensionName.data());
		}

		return OutDeviceExtensions;
	}

	auto FVulkanDeviceExtension::GetDriverSupportedDeviceExtensions(vk::PhysicalDevice Gpu, const char* LayerName) -> std::vector<vk::ExtensionProperties>
	{
		if (LayerName == nullptr)
		{
			return Gpu.enumerateDeviceExtensionProperties(nullptr);
		}
		return Gpu.enumerateDeviceExtensionProperties(std::string(LayerName));
	}
}
