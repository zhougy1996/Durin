#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanLayout;

	// Accumulates descriptor counts required to allocate a compatible Vulkan pool.
	struct FVulkanDescriptorRequirements
	{
		uint32 MaxSets = 0;
		std::unordered_map<vk::DescriptorType, uint32> DescriptorCounts;

		auto Add(const FVulkanDescriptorRequirements& Other) -> void
		{
			MaxSets += Other.MaxSets;
			for (const auto& [Type, Count] : Other.DescriptorCounts)
			{
				DescriptorCounts[Type] += Count;
			}
		}
	};

	// Describes the array of descriptor set layouts which will be used to create pipeline layouts.
	// Only describe the information, does not hold any runtime object
	class FVulkanDescriptorSetsLayoutInfo
	{
	public:
		FVulkanDescriptorSetsLayoutInfo() = default;
		FVulkanDescriptorSetsLayoutInfo(const std::vector<FBindingLayout>& InBindingLayouts);

		// Describes bindings and allocation requirements for one descriptor-set index.
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

				if (!LayoutBindings.empty() && std::memcmp(LayoutBindings.data(), Other.LayoutBindings.data(), LayoutBindings.size() * sizeof(vk::DescriptorSetLayoutBinding)) != 0)
				{
					return false;
				}

				return true;
			}
		};

		// Hashes one descriptor-set layout for structural cache lookup.
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

		auto GetDescriptorRequirements() const -> FVulkanDescriptorRequirements;

		auto GetHash() const -> uint64 { return Hash.HashValue; }

		auto operator==( const FVulkanDescriptorSetsLayoutInfo& Other) const -> bool
		{
			if (Hash != Other.Hash || SetLayouts.size() != Other.SetLayouts.size())
			{
				return false;
			}

			for (size_t i = 0; i < SetLayouts.size(); ++i)
			{
				if (SetLayouts[i] != Other.SetLayouts[i])
				{
					return false;
				}
			}

			return true;
		}

		FVulkanDescriptorSetsLayoutInfo(const FVulkanDescriptorSetsLayoutInfo& Other) = default;

	private:
		void GenerateHash();

		// The number of descriptors of each type in the layout, used for descriptor pool creation
		std::unordered_map<vk::DescriptorType, uint32> LayoutTypes;

		std::vector<FSetLayout> SetLayouts;

		FXxHash64 Hash;

		friend std::hash<FVulkanDescriptorSetsLayoutInfo>;
	};

	// Owns one cached Vulkan descriptor-set layout and its allocation requirements.
	struct FVulkanDescriptorSetLayoutEntry
	{
		// The Vulkan descriptor set layout handle
		vk::DescriptorSetLayout Handle{};
		// The unique ID of the layout, in case we want to judge whether two layouts are the same, since the handle may be reused by Vulkan after one of them is destroyed and recreated
		uint64 HandleId = 0;
	};

	// Reuses Vulkan descriptor-set layouts with identical structural descriptions.
	class FVulkanDescriptorSetLayoutCache
	{
	public:
		explicit FVulkanDescriptorSetLayoutCache(FVulkanDevice& InDevice) : Device(InDevice) {}

		~FVulkanDescriptorSetLayoutCache();

		auto GetOrCreateDescriptorSetLayout(const FVulkanDescriptorSetsLayoutInfo::FSetLayout& Layout) -> vk::DescriptorSetLayout;

	private:
		using FVulkanDescriptorSetLayoutMap = std::unordered_map<FVulkanDescriptorSetsLayoutInfo::FSetLayout, FVulkanDescriptorSetLayoutEntry, FVulkanDescriptorSetsLayoutInfo::FSetLayoutHasher>;

		FVulkanDevice& Device;

		// Will not remove any entry from the cache, even when the layout is destroyed, since the handle may be reused by Vulkan and cause confusion. The cache will only be cleared when the device is destroyed.
		FVulkanDescriptorSetLayoutMap DLayoutMap;

		std::mutex Mutex;
	};

	// The runtime object of descriptor set layouts
	class FVulkanDescriptorSetsLayout
	{
	public:
		FVulkanDescriptorSetsLayout(FVulkanDevice& InDevice)
			: Device(InDevice)
		{
		}
		explicit FVulkanDescriptorSetsLayout(FVulkanDevice& InDevice, FVulkanDescriptorSetsLayoutInfo InInfo);

		~FVulkanDescriptorSetsLayout() = default;

		auto GetInfo() const -> const FVulkanDescriptorSetsLayoutInfo& { return Info; }

		auto GetLayoutHandles() const -> const std::vector<vk::DescriptorSetLayout>& { return LayoutHandles; }

		auto GetHash() const -> uint64 { return Info.GetHash(); }

		FVulkanDescriptorSetsLayout(const FVulkanDescriptorSetsLayout& Other) = default;
		auto operator=(const FVulkanDescriptorSetsLayout& Other) -> FVulkanDescriptorSetsLayout&
		{
			if (this != &Other)
			{
				Info = Other.Info;
				LayoutHandles = Other.LayoutHandles;
			}
			return *this;
		}

	private:
		FVulkanDevice& Device;

		FVulkanDescriptorSetsLayoutInfo Info;

		std::vector<vk::DescriptorSetLayout> LayoutHandles;
	};

	// Owns a Vulkan pipeline layout together with its descriptor-set layouts.
	class FVulkanLayout
	{
	public:
		FVulkanLayout(FVulkanDevice& InDevice)
			: Device(InDevice)
			, DSetsLayout(InDevice)
		{
		}

		auto GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout& { return DSetsLayout; }

	private:
		FVulkanDevice& Device;
		FVulkanDescriptorSetsLayout DSetsLayout;

		friend class FVulkanPipelineManager;
	};

	// Allocates descriptor sets from one Vulkan pool with bounded capacity.
	class FVulkanDescriptorPool
	{
	public:
		FVulkanDescriptorPool(FVulkanDevice* InDevice, const FVulkanDescriptorRequirements& InRequirements);
		~FVulkanDescriptorPool();

		auto GetHandle() const -> vk::DescriptorPool { return DescriptorPool; }
		auto GetMaxSets() const -> uint32 { return MaxDescriptorSets; }
		auto GetAllocatedSets() const -> uint32 { return NumAllocatedDescriptorSets; }
		auto GetPeakAllocatedSets() const -> uint32 { return PeakAllocatedDescriptorSets; }
		auto GetDescriptorCapacity(vk::DescriptorType Type) const -> uint32;
		auto CanAllocate(const FVulkanDescriptorRequirements& Requirements) const -> bool;
		auto CommitAllocation(const FVulkanDescriptorRequirements& Requirements) -> void;
		auto Reset() -> void;

	private:
		FVulkanDevice* Device;

		vk::DescriptorPool DescriptorPool;

		uint32 MaxDescriptorSets;
		uint32 NumAllocatedDescriptorSets;
		uint32 PeakAllocatedDescriptorSets;
		std::unordered_map<vk::DescriptorType, uint32> DescriptorCapacities;
		std::unordered_map<vk::DescriptorType, uint32> NumAllocatedDescriptors;
	};

	// Reuses descriptor sets whose bound-resource identity remains unchanged.
	class FVulkanDescriptorSetCache
	{
	public:
		explicit FVulkanDescriptorSetCache(FVulkanDevice* InDevice);

	private:
		FVulkanDevice* Device;
	};

	// Coordinates descriptor allocation and caching across frames for one device.
	class FVulkanGlobalDescriptorPool
	{
	public:
		explicit FVulkanGlobalDescriptorPool(FVulkanDevice& InDevice);

		~FVulkanGlobalDescriptorPool();

		auto AllocateDescriptorSets(
			std::span<const vk::DescriptorSetLayout> Layouts,
			const FVulkanDescriptorRequirements& Requirements
		) -> std::vector<vk::DescriptorSet>;

		auto ResetPoolsForCurrentFrame() -> void;

	private:
		auto CreatePool(uint32 FrameIndex, const FVulkanDescriptorRequirements& Requirements, uint32 GrowthMaxSets) -> FVulkanDescriptorPool&;
		auto GetCurrentPools() -> std::vector<std::unique_ptr<FVulkanDescriptorPool>>&;

		FVulkanDevice& Device;

		std::array<std::vector<std::unique_ptr<FVulkanDescriptorPool>>, kFrameInFlight> Pools;
		std::array<uint32, kFrameInFlight> PoolExpansions = {};
	};
} // namespace Durin::VulkanRHI

template<>
struct std::hash<Durin::VulkanRHI::FVulkanDescriptorSetsLayoutInfo>
{
	size_t operator()(const Durin::VulkanRHI::FVulkanDescriptorSetsLayoutInfo& Info) const noexcept
	{
		return Info.Hash.HashValue;
	}
};
