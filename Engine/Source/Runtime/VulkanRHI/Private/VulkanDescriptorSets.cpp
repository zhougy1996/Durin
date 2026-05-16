#include "VulkanDescriptorSets.h"

#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	static constexpr std::array DescriptorTypes = {
		vk::DescriptorType::eSampler,
		vk::DescriptorType::eCombinedImageSampler,
		vk::DescriptorType::eSampledImage,
		vk::DescriptorType::eStorageImage,
		vk::DescriptorType::eUniformTexelBuffer,
		vk::DescriptorType::eStorageTexelBuffer,
		vk::DescriptorType::eUniformBuffer,
		vk::DescriptorType::eStorageBuffer,
		vk::DescriptorType::eUniformBufferDynamic,
		vk::DescriptorType::eStorageBufferDynamic,
		vk::DescriptorType::eInputAttachment,
		vk::DescriptorType::eAccelerationStructureKHR
	};

	static const std::unordered_map<vk::DescriptorType, uint32> GEmptyLayoutTypes = []() {
		std::unordered_map<vk::DescriptorType, uint32> result;
		result.reserve(DescriptorTypes.size());
		for (vk::DescriptorType Type : DescriptorTypes)
		{
			result[Type] = 0;
		}
		return result;
	}();

	FVulkanDescriptorSetsLayoutInfo::FVulkanDescriptorSetsLayoutInfo()
		: LayoutTypes(GEmptyLayoutTypes)
	{
	}

	void FVulkanDescriptorSetsLayoutInfo::GenerateHash()
	{
	}

	FVulkanDescriptorSetLayoutCache::~FVulkanDescriptorSetLayoutCache()
	{
		for (const auto& Entry : DLayoutMap | std::views::values)
		{
			Device->GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::DescriptorSetLayout, Entry.Handle);
		}
	}

	auto FVulkanDescriptorSetLayoutCache::GetOrCreateDescriptorSetLayout(const FVulkanDescriptorSetsLayoutInfo::FSetLayout& Layout) -> vk::DescriptorSetLayout
	{
		std::lock_guard Lock(Mutex);

		if (const auto It = DLayoutMap.find(Layout); It != DLayoutMap.end())
		{
			const FVulkanDescriptorSetLayoutEntry& FoundEntry = It->second;
			return FoundEntry.Handle;
		}
		// Create a new descriptor set layout if it doesn't exist in the cache
		vk::DescriptorSetLayoutCreateInfo CreateInfo{};
		CreateInfo.setBindings(Layout.LayoutBindings);

		FVulkanDescriptorSetLayoutEntry NewEntry;
		NewEntry.Handle = Device->GetHandle().createDescriptorSetLayout(CreateInfo);
		NewEntry.HandleId = ++GVulkanDSetLayoutHandleIdCounter;
		DLayoutMap[Layout] = NewEntry;

		return NewEntry.Handle;
	}

	FVulkanDescriptorSetsLayout::FVulkanDescriptorSetsLayout(FVulkanDevice* InDevice, FVulkanDescriptorSetsLayoutInfo InInfo)
		: Device(InDevice)
		, Info(std::move(InInfo))
	{
	}

	FVulkanDescriptorSetCache::FVulkanDescriptorSetCache(FVulkanDevice* InDevice)
		: Device(InDevice)
	{
	}

	FVulkanGlobalDescriptorPool::FVulkanGlobalDescriptorPool(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		for (uint32 i = 0; i < kFrameInFlight; ++i)
		{
			Pools[i] = CreatePool();
		}
	}

	FVulkanGlobalDescriptorPool::~FVulkanGlobalDescriptorPool()
	{
		for (const auto& Pool : Pools)
		{
			Device.GetHandle().destroyDescriptorPool(Pool);
		}
	}

	auto FVulkanGlobalDescriptorPool::GetPool() const -> vk::DescriptorPool
	{
		return Pools[GFrameCounterRenderThread % Pools.size()];
	}

	auto FVulkanGlobalDescriptorPool::ResetPoolsForCurrentFrame() const -> void
	{
		Device.GetHandle().resetDescriptorPool(Pools[GFrameCounterRenderThread % Pools.size()]);
	}

	auto FVulkanGlobalDescriptorPool::CreatePool() -> vk::DescriptorPool
	{
		constexpr auto MaxSets = 256;

		std::vector<vk::DescriptorPoolSize> PoolSizes;
		PoolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, MaxSets});
		PoolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, MaxSets});
		PoolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eSampler, MaxSets});

		vk::DescriptorPoolCreateInfo DescriptorPoolCreateInfo;
		DescriptorPoolCreateInfo
			.setPoolSizes(PoolSizes)
			.setMaxSets(MaxSets)
			.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);

		return Device.GetHandle().createDescriptorPool(DescriptorPoolCreateInfo);
	}
} // namespace Doge::VulkanRHI