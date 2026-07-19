#include "VulkanDescriptorSets.h"

#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
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
				LayoutTypes[ToVulkan_RHIBindingType(InBinding.Type)] += InBinding.ArraySize;
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

	auto FVulkanDescriptorSetsLayoutInfo::GetDescriptorRequirements() const -> FVulkanDescriptorRequirements
	{
		FVulkanDescriptorRequirements Requirements;
		Requirements.MaxSets = static_cast<uint32>(SetLayouts.size());
		Requirements.DescriptorCounts = LayoutTypes;
		return Requirements;
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
	}

	FVulkanGlobalDescriptorPool::~FVulkanGlobalDescriptorPool()
	{
	}

	FVulkanDescriptorPool::FVulkanDescriptorPool(FVulkanDevice* InDevice, const FVulkanDescriptorRequirements& InRequirements)
		: Device(InDevice)
		, MaxDescriptorSets(std::max(256u, InRequirements.MaxSets * 2u))
		, NumAllocatedDescriptorSets(0)
		, PeakAllocatedDescriptorSets(0)
	{
		std::vector<vk::DescriptorPoolSize> PoolSizes;
		PoolSizes.reserve(InRequirements.DescriptorCounts.size());
		for (const auto& [Type, Count] : InRequirements.DescriptorCounts)
		{
			const uint32 Capacity = std::max(256u, Count * 2u);
			DescriptorCapacities.emplace(Type, Capacity);
			PoolSizes.emplace_back(Type, Capacity);
		}

		vk::DescriptorPoolCreateInfo CreateInfo;
		CreateInfo
			.setPoolSizes(PoolSizes)
			.setMaxSets(MaxDescriptorSets);
		DescriptorPool = Device->GetHandle().createDescriptorPool(CreateInfo);
	}

	FVulkanDescriptorPool::~FVulkanDescriptorPool()
	{
		if (DescriptorPool)
		{
			Device->GetHandle().destroyDescriptorPool(DescriptorPool);
		}
	}

	auto FVulkanDescriptorPool::GetDescriptorCapacity(vk::DescriptorType Type) const -> uint32
	{
		if (const auto It = DescriptorCapacities.find(Type); It != DescriptorCapacities.end())
		{
			return It->second;
		}
		return 0;
	}

	auto FVulkanDescriptorPool::CanAllocate(const FVulkanDescriptorRequirements& Requirements) const -> bool
	{
		if (NumAllocatedDescriptorSets + Requirements.MaxSets > MaxDescriptorSets)
		{
			return false;
		}
		for (const auto& [Type, Count] : Requirements.DescriptorCounts)
		{
			const auto Capacity = GetDescriptorCapacity(Type);
			const auto Used = NumAllocatedDescriptors.contains(Type) ? NumAllocatedDescriptors.at(Type) : 0;
			if (Capacity == 0 || Used + Count > Capacity)
			{
				return false;
			}
		}
		return true;
	}

	auto FVulkanDescriptorPool::CommitAllocation(const FVulkanDescriptorRequirements& Requirements) -> void
	{
		NumAllocatedDescriptorSets += Requirements.MaxSets;
		PeakAllocatedDescriptorSets = std::max(PeakAllocatedDescriptorSets, NumAllocatedDescriptorSets);
		for (const auto& [Type, Count] : Requirements.DescriptorCounts)
		{
			NumAllocatedDescriptors[Type] += Count;
		}
	}

	auto FVulkanDescriptorPool::Reset() -> void
	{
		Device->GetHandle().resetDescriptorPool(DescriptorPool);
		NumAllocatedDescriptorSets = 0;
		NumAllocatedDescriptors.clear();
	}

	auto FVulkanGlobalDescriptorPool::GetCurrentPools() -> std::vector<std::unique_ptr<FVulkanDescriptorPool>>&
	{
		return Pools[GRenderFrameCounterRenderThread % Pools.size()];
	}

	auto FVulkanGlobalDescriptorPool::CreatePool(uint32 FrameIndex, const FVulkanDescriptorRequirements& Requirements, uint32 GrowthMaxSets) -> FVulkanDescriptorPool&
	{
		constexpr uint32 MaxPoolsPerFrame = 32;
		FVulkanDescriptorRequirements PoolRequirements = Requirements;
		PoolRequirements.MaxSets = std::max(PoolRequirements.MaxSets, GrowthMaxSets);
		for (auto& [Type, Count] : PoolRequirements.DescriptorCounts)
		{
			Count = std::max(Count, 128u);
		}

		auto& FramePools = Pools[FrameIndex];
		checkf(FramePools.size() < MaxPoolsPerFrame,
			"Vulkan descriptor pool expansion limit reached: frame={}, poolCount={}, requestedSets={}",
			FrameIndex, FramePools.size(), Requirements.MaxSets);
		const bool bIsExpansion = !FramePools.empty();
		FramePools.push_back(std::make_unique<FVulkanDescriptorPool>(&Device, PoolRequirements));
		if (bIsExpansion)
		{
			++PoolExpansions[FrameIndex];
		}
		DURIN_DEBUG("Created Vulkan descriptor pool: frame={}, poolCount={}, maxSets={}, expansions={}",
			FrameIndex, FramePools.size(), FramePools.back()->GetMaxSets(), PoolExpansions[FrameIndex]);
		return *FramePools.back();
	}

	auto FVulkanGlobalDescriptorPool::AllocateDescriptorSets(
		std::span<const vk::DescriptorSetLayout> Layouts,
		const FVulkanDescriptorRequirements& Requirements
	) -> std::vector<vk::DescriptorSet>
	{
		if (Layouts.empty())
		{
			return {};
		}

		auto& FramePools = GetCurrentPools();
		for (const auto& Pool : FramePools)
		{
			if (!Pool->CanAllocate(Requirements))
			{
				continue;
			}

			vk::DescriptorSetAllocateInfo AllocateInfo;
			AllocateInfo
				.setDescriptorPool(Pool->GetHandle())
				.setSetLayouts(Layouts);
			try
			{
				auto DescriptorSets = Device.GetHandle().allocateDescriptorSets(AllocateInfo);
				Pool->CommitAllocation(Requirements);
				return DescriptorSets;
			}
			catch (const vk::SystemError& Error)
			{
				const auto Result = static_cast<vk::Result>(Error.code().value());
				if (Result != vk::Result::eErrorOutOfPoolMemory && Result != vk::Result::eErrorFragmentedPool)
				{
					throw;
				}
			}
		}

		const uint32 FrameIndex = static_cast<uint32>(GRenderFrameCounterRenderThread % Pools.size());
		uint32 GrowthMaxSets = 256;
		if (!FramePools.empty())
		{
			GrowthMaxSets = FramePools.back()->GetMaxSets() * 2u;
		}
		FVulkanDescriptorPool& NewPool = CreatePool(FrameIndex, Requirements, GrowthMaxSets);
		vk::DescriptorSetAllocateInfo AllocateInfo;
		AllocateInfo
			.setDescriptorPool(NewPool.GetHandle())
			.setSetLayouts(Layouts);
		try
		{
			auto DescriptorSets = Device.GetHandle().allocateDescriptorSets(AllocateInfo);
			NewPool.CommitAllocation(Requirements);
			return DescriptorSets;
		}
		catch (const vk::SystemError& Error)
		{
			DURIN_ERROR("Failed to allocate Vulkan descriptor sets after pool expansion: result={}, sets={}, descriptorTypes={}",
				vk::to_string(static_cast<vk::Result>(Error.code().value())), Requirements.MaxSets, Requirements.DescriptorCounts.size());
			throw;
		}
	}

	auto FVulkanGlobalDescriptorPool::ResetPoolsForCurrentFrame() -> void
	{
		for (const auto& Pool : GetCurrentPools())
		{
			Pool->Reset();
		}
	}
} // namespace Durin::VulkanRHI
