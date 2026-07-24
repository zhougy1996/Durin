#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanInstanceExtension;
	class FVulkanDeviceExtension;

	// Describes an optional Vulkan extension and hooks for feature negotiation.
	class FVulkanExtensionBase
	{
	public:
		FVulkanExtensionBase(const char* InExtensionName)
			: ExtensionName(InExtensionName)
		{
		}

		auto SetSupported() -> void { bSupported = true; }
		auto SetActivated() -> void { bActivated = true; }

		auto IsSupported() const -> bool { return bSupported; }
		auto GetExtensionName() const -> const char* { return ExtensionName; }

		auto InUse() const -> bool;

	protected:
		const char* ExtensionName;

		bool bSupported = false;

		bool bActivated = false;
	};

	inline auto FVulkanExtensionBase::InUse() const -> bool
	{
		return bSupported && bActivated;
	}

	using FVulkanInstanceExtensionArray = std::vector<std::unique_ptr<FVulkanInstanceExtension>>;
	using FVulkanDeviceExtensionArray = std::vector<std::unique_ptr<FVulkanDeviceExtension>>;

	// Specializes extension negotiation for Vulkan instance creation.
	class FVulkanInstanceExtension : public FVulkanExtensionBase
	{
	public:
		static auto GetDurinSupportedInstanceExtensions() -> FVulkanInstanceExtensionArray;
	};

	// Specializes extension negotiation for logical-device creation.
	class FVulkanDeviceExtension : public FVulkanExtensionBase
	{
	public:
		static auto GetDurinSupportedDeviceExtensions(FVulkanDevice* InDevice) -> FVulkanDeviceExtensionArray;
		static auto GetDriverSupportedDeviceExtensions(vk::PhysicalDevice Gpu, const char* LayerName = nullptr) -> std::vector<vk::ExtensionProperties>;

	protected:
	};
}
