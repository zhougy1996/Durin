#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	// Describes the array of descriptor set layouts which will be used to create pipeline layouts.
	// Only describe the information, does not hold any runtime object
	class FVulkanDescriptorSetsLayoutInfo
	{
	public:
		FVulkanDescriptorSetsLayoutInfo();

		struct FSetLayout
		{
			std::vector<vk::DescriptorSetLayoutBinding> LayoutBindings;
			FXxHash64 Hash;

			auto GenerateHash() -> void
			{
				Hash = FXxHash64::HashBuffer(LayoutBindings.data(), LayoutBindings.size() * sizeof(vk::DescriptorSetLayoutBinding));
			}

			auto operator==(const FSetLayout& Other) const -> bool
			{
				if (Hash != Other.Hash || LayoutBindings.size() != Other.LayoutBindings.size())
				{
					return false;
				}

				if (!LayoutBindings.empty() && !std::memcmp(LayoutBindings.data(), Other.LayoutBindings.data(), LayoutBindings.size() * sizeof(vk::DescriptorSetLayoutBinding)))
				{
					return false;
				}

				return true;
			}
		};

		struct FSetLayoutHasher
		{
			auto operator()(const FSetLayout& Layout) const -> size_t
			{
				return Layout.Hash.HashValue;
			}
		};

		auto GetTypeUsedCount(vk::DescriptorType Type) const -> uint32
		{
			if (const auto It = LayoutTypes.find(Type); It != LayoutTypes.end())
			{
				return It->second;
			}
			return 0;
		}

		auto GetLayouts() const -> const std::vector<FSetLayout>& { return SetLayouts; }

		void GenerateHash();

	private:
		// The number of descriptors of each type in the layout, used for descriptor pool creation
		std::unordered_map<vk::DescriptorType, uint32> LayoutTypes;

		std::vector<FSetLayout> SetLayouts;

		FXxHash64 Hash;

		friend std::hash<FVulkanDescriptorSetsLayoutInfo>;
	};

	struct FVulkanDescriptorSetLayoutEntry
	{
		// The Vulkan descriptor set layout handle
		vk::DescriptorSetLayout Handle{};
		// The unique ID of the layout, in case we want to judge whether two layouts are the same, since the handle may be reused by Vulkan after one of them is destroyed and recreated
		uint64 HandleId = 0;
	};

	class FVulkanDescriptorSetLayoutCache
	{
	public:
		FVulkanDescriptorSetLayoutCache(FVulkanDevice* InDevice) : Device(InDevice) {}

		~FVulkanDescriptorSetLayoutCache();

		auto GetOrCreateDescriptorSetLayout(const FVulkanDescriptorSetsLayoutInfo::FSetLayout& Layout) -> vk::DescriptorSetLayout;

	private:
		using FVulkanDescriptorSetLayoutMap = std::unordered_map<FVulkanDescriptorSetsLayoutInfo::FSetLayout, FVulkanDescriptorSetLayoutEntry, FVulkanDescriptorSetsLayoutInfo::FSetLayoutHasher>;

		FVulkanDevice* Device;

		FVulkanDescriptorSetLayoutMap DLayoutMap;

		std::mutex Mutex;
	};

	// The runtime object of descriptor set layouts, created from FVulkanDescriptorSetsLayoutInfo
	class FVulkanDescriptorSetsLayout
	{
	public:
		FVulkanDescriptorSetsLayout(FVulkanDescriptorSetsLayoutInfo InInfo);

		auto GetInfo() const -> const FVulkanDescriptorSetsLayoutInfo& { return Info; }
	private:
		FVulkanDescriptorSetsLayoutInfo Info;
	};

}

template<>
struct std::hash<Doge::VulkanRHI::FVulkanDescriptorSetsLayoutInfo>
{
	size_t operator()(const Doge::VulkanRHI::FVulkanDescriptorSetsLayoutInfo& Info) const noexcept
	{
		return Info.Hash.HashValue;
	}
};

