#include "VulkanDescriptorSets.h"

#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
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

	FVulkanDescriptorSetsLayoutInfo::FVulkanDescriptorSetsLayoutInfo(const std::vector<FBindingLayout>& InBindingLayouts)
	{
		for (const auto& InBindingLayout : InBindingLayouts)
		{
			FSetLayout SetLayout;
			for (const auto& InBinding : InBindingLayout.BindingLayouts)
			{
				vk::DescriptorSetLayoutBinding LayoutBinding{};
				LayoutBinding.binding = InBinding.Slot;
				LayoutBinding.descriptorType = ToVulkan_RHIBindingType(InBinding.Type);
				LayoutBinding.descriptorCount = InBinding.ArraySize;
				LayoutBinding.stageFlags = ToVulkan_ShaderStageFlags(InBinding.StageFlags);

				SetLayout.LayoutBindings.push_back(LayoutBinding);
				LayoutTypes[ToVulkan_RHIBindingType(InBinding.Type)] += 1;
			}
			std::ranges::sort(SetLayout.LayoutBindings, [](const vk::DescriptorSetLayoutBinding& A, const vk::DescriptorSetLayoutBinding& B) {
				return A.binding < B.binding;
			});
			SetLayout.GenerateHash();
			SetLayouts.push_back(std::move(SetLayout));
		}
		GenerateHash();
	}

	FVulkanDescriptorSetLayoutCache::~FVulkanDescriptorSetLayoutCache()
	{
		for (const auto& Entry : DLayoutMap | std::views::values)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::DescriptorSetLayout, Entry.Handle);
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
		NewEntry.Handle = Device.GetHandle().createDescriptorSetLayout(CreateInfo);
		NewEntry.HandleId = ++GVulkanDSetLayoutHandleIdCounter;
		DLayoutMap[Layout] = NewEntry;

		return NewEntry.Handle;
	}

	void FVulkanDescriptorSetsLayoutInfo::GenerateHash()
	{
		FXxHash64Builder HashBuilder;
		for (const auto& SetLayout : SetLayouts)
		{
			HashBuilder.UpdateValue(SetLayout.Hash);
		}
		Hash = HashBuilder.Finalize();
	}

	FVulkanDescriptorSetsLayout::FVulkanDescriptorSetsLayout(FVulkanDevice& InDevice, FVulkanDescriptorSetsLayoutInfo InInfo)
		: Device(InDevice)
		, Info(std::move(InInfo))
	{
		for (const auto& SetLayout : Info.GetLayouts())
		{
			LayoutHandles.push_back(Device.GetDescriptorSetLayoutCache().GetOrCreateDescriptorSetLayout(SetLayout));
		}
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
		return Pools[GRenderFrameCounterRenderThread % Pools.size()];
	}

	auto FVulkanGlobalDescriptorPool::ResetPoolsForCurrentFrame() const -> void
	{
		Device.GetHandle().resetDescriptorPool(Pools[GRenderFrameCounterRenderThread % Pools.size()]);
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
} // namespace Durin::VulkanRHI
