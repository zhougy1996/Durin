#include "VulkanExtensions.h"

#include "RHIGlobals.h"

inline constexpr const char* DogeSupportedInstanceExtensionNames[] = {
	VK_KHR_SURFACE_EXTENSION_NAME,
	VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

inline constexpr const char* DogeSupportedDeviceExtensionNames[] = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME};

template<typename ExtensionType>
static auto FindExtension(const TArray<TUniquePtr<ExtensionType>>& Extensions, const char* ExtensionName) -> int32
{
	for (int32 Index = 0; Index < Extensions.size(); ++Index)
	{
		if (strcmp(ExtensionName, Extensions[Index]->GetExtensionName()) == 0)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

template<typename ExtensionType>
static auto AddRequiredExtentions(TArray<TUniquePtr<ExtensionType>>& Extensions, const TArray<const char*>& RequiredExtentionNames)
{
	for (const char* ExtensionName : RequiredExtentionNames)
	{
		auto it = std::find_if(Extensions.begin(), Extensions.end(), [ExtensionName](TUniquePtr<ExtensionType>& Extention) {
			return Extention->GetExtensionName() == ExtensionName;
		});

		if (it == Extensions.end())
		{
			Extensions.push_back(std::make_unique<ExtensionType>(ExtensionName));
		}
	}
}

auto FVulkanInstanceExtension::GetDogeSupportedInstanceExtensions() -> FVulkanInstanceExtensionArray
{
	FVulkanInstanceExtensionArray OutDogeInstanceExtensions;

	for (const char* ExtensionName : DogeSupportedInstanceExtensionNames)
	{
		OutDogeInstanceExtensions.push_back(std::make_unique<FVulkanInstanceExtension>(ExtensionName));
	}
	AddRequiredExtentions(OutDogeInstanceExtensions, GKleeRequiredVulkanInstanceExtensions);

	TArray<vk::ExtensionProperties> DriverSupportedInstanceExtensions = vk::enumerateInstanceExtensionProperties();
	DOGE_DEBUG("Found {} available instance extensions:", DriverSupportedInstanceExtensions.size());
	for (const vk::ExtensionProperties& Extension : DriverSupportedInstanceExtensions)
	{
		const int32 ExtensionIndex = FindExtension(OutDogeInstanceExtensions, Extension.extensionName);
		const bool bFound = (ExtensionIndex != INDEX_NONE);
		if (bFound)
		{
			// Set the extension as supported and activated temporarily.
			// TODO: some extensions may not be activated by default.
			OutDogeInstanceExtensions[ExtensionIndex]->SetSupported();
			OutDogeInstanceExtensions[ExtensionIndex]->SetActivated();
		}
		DOGE_DEBUG("{} {}", bFound ? "+" : "-", Extension.extensionName.data());
	}

	return OutDogeInstanceExtensions;
}

auto FVulkanDeviceExtension::GetDogeSupportedDeviceExtensions(FVulkanDevice* Device) -> FVulkanDeviceExtensionArray
{
	FVulkanDeviceExtensionArray OutDeviceExtensions;

	for (const char* ExtensionName : DogeSupportedDeviceExtensionNames)
	{
		OutDeviceExtensions.push_back(std::make_unique<FVulkanDeviceExtension>(ExtensionName));
	}

	TArray<vk::ExtensionProperties> DriverSupportedDeviceExtensions/* = GetDriverSupportedDeviceExtensions(Device->GetGpu())*/;
	DOGE_DEBUG("Found {} available device extensions:", DriverSupportedDeviceExtensions.size());

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
		DOGE_DEBUG("{} {}", bFound ? "+" : "-", Extension.extensionName.data());
	}

	return OutDeviceExtensions;
}

auto FVulkanDeviceExtension::GetDriverSupportedDeviceExtensions(vk::PhysicalDevice Gpu, const char* LayerName) -> TArray<vk::ExtensionProperties>
{
	if (LayerName == nullptr)
	{
		return Gpu.enumerateDeviceExtensionProperties(nullptr);
	}
	return Gpu.enumerateDeviceExtensionProperties(std::string(LayerName));
}
