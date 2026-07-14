#include "VulkanExtensions.h"

#include "CoreGlobals.h"

#include "VulkanDevice.h"

#ifdef __APPLE__
	#define VKB_ENABLE_PORTABILITY
#endif

namespace Durin::VulkanRHI
{
	namespace
	{
		template<typename ExtensionType>
		auto JoinExtensionNames(const std::vector<std::unique_ptr<ExtensionType>>& Extensions, const bool bInUse) -> std::string
		{
			std::string Result;
			for (const auto& Extension : Extensions)
			{
				if (Extension->InUse() != bInUse) continue;
				if (!Result.empty()) Result += ", ";
				Result += Extension->GetExtensionName();
			}
			return Result.empty() ? "none" : Result;
		}

		template<typename ExtensionType>
		auto LogExtensionSupport(std::string_view Scope, const std::vector<std::unique_ptr<ExtensionType>>& Extensions) -> void
		{
			const size_t EnabledCount = std::ranges::count_if(Extensions, [](const auto& Extension) { return Extension->InUse(); });
			DURIN_TRACE("Vulkan {} extensions: requested={}, enabled={} [{}], missing={} [{}].", Scope, Extensions.size(),
				EnabledCount, JoinExtensionNames(Extensions, true), Extensions.size() - EnabledCount, JoinExtensionNames(Extensions, false));
		}
	}

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
			auto it = std::find_if(Extensions.begin(), Extensions.end(), [ExtensionName](const std::unique_ptr<ExtensionType>& Extension) {
				return strcmp(Extension->GetExtensionName(), ExtensionName) == 0;
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
		}
		LogExtensionSupport("instance", OutDurinInstanceExtensions);

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
		}
		LogExtensionSupport("device", OutDeviceExtensions);

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
