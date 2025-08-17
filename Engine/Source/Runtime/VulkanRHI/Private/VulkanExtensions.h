#pragma once

class FVulkanDevice;
class FVulkanInstanceExtension;
class FVulkanDeviceExtension;

class FVulkanExtensionBase
{
public:
	FVulkanExtensionBase(const char* ExtensionName)
		: ExtensionName_(ExtensionName)
	{
	}

	auto SetSupported() -> void { bSupported_ = true; }
	auto SetActivated() -> void { bActivated_ = true; }

	auto IsSupported() const -> bool { return bSupported_; }
	auto GetExtensionName() const -> const char* { return ExtensionName_; }

	auto InUse() const -> bool;

protected:
	const char* ExtensionName_;

	bool bSupported_ = false;

	bool bActivated_ = false;
};

inline auto FVulkanExtensionBase::InUse() const -> bool
{
	return bSupported_ && bActivated_;
}

using FVulkanInstanceExtensionArray = std::vector<TUniquePtr<FVulkanInstanceExtension>>;
using FVulkanDeviceExtensionArray = std::vector<TUniquePtr<FVulkanDeviceExtension>>;

class FVulkanInstanceExtension : public FVulkanExtensionBase
{
public:
	static auto GetDogeSupportedInstanceExtensions() -> FVulkanInstanceExtensionArray;
};

class FVulkanDeviceExtension : public FVulkanExtensionBase
{
public:
	static auto GetDogeSupportedDeviceExtensions(FVulkanDevice* Device) -> FVulkanDeviceExtensionArray;
	static auto GetDriverSupportedDeviceExtensions(vk::PhysicalDevice Gpu, const char* LayerName = nullptr) -> std::vector<vk::ExtensionProperties>;

protected:
};
