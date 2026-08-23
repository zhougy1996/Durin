#include "VulkanDescriptorSets.h"

#include "VulkanDevice.h"
#include "VulkanCompletion.h"
#include "VulkanDiagnostics.h"
#include "VulkanDynamicRHI.h"
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
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanDescriptorSetLayoutEntryCount.fetch_sub(DLayoutMap.size(), std::memory_order_release);
#endif
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
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::DescriptorSetLayout);
#endif
		NewEntry.Handle = Device.GetHandle().createDescriptorSetLayout(CreateInfo);
		Device.GetRHI().GetDebugUtils().NameObject(NewEntry.Handle,
			Device.GetRHI().GetDebugUtils().MakeInternalName("DescriptorSetLayout"));
		NewEntry.HandleId = ++GVulkanDSetLayoutHandleIdCounter;
		const auto [InsertedIt, bInserted] = DLayoutMap.emplace(Layout, NewEntry);
		check(bInserted);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanDescriptorSetLayoutEntryCount.fetch_add(1, std::memory_order_release);
#endif

		return InsertedIt->second.Handle;
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
		, MaxDescriptorSets(InRequirements.MaxSets)
		, NumAllocatedDescriptorSets(0)
		, PeakAllocatedDescriptorSets(0)
	{
		std::vector<vk::DescriptorPoolSize> PoolSizes;
		PoolSizes.reserve(InRequirements.DescriptorCounts.size());
		for (const auto& [Type, Count] : InRequirements.DescriptorCounts)
		{
			DescriptorCapacities.emplace(Type, Count);
			PoolSizes.emplace_back(Type, Count);
		}

		vk::DescriptorPoolCreateInfo CreateInfo;
		CreateInfo
			.setPoolSizes(PoolSizes)
			.setMaxSets(MaxDescriptorSets);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::DescriptorPool);
#endif
		DescriptorPool = Device->GetHandle().createDescriptorPool(CreateInfo);
		Device->GetRHI().GetDebugUtils().NameObject(DescriptorPool,
			Device->GetRHI().GetDebugUtils().MakeInternalName("DescriptorPool"));
		GVulkanMemoryBaselineTracker.RecordDescriptorPoolCreated(MaxDescriptorSets);
	}

	FVulkanDescriptorPool::~FVulkanDescriptorPool()
	{
		if (DescriptorPool)
		{
			GVulkanMemoryBaselineTracker.RecordDescriptorPoolDestroyed(
				MaxDescriptorSets, NumAllocatedDescriptorSets);
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
		GVulkanMemoryBaselineTracker.RecordDescriptorSetsAllocated(
			Requirements.MaxSets);
		PeakAllocatedDescriptorSets = std::max(PeakAllocatedDescriptorSets, NumAllocatedDescriptorSets);
		for (const auto& [Type, Count] : Requirements.DescriptorCounts)
		{
			NumAllocatedDescriptors[Type] += Count;
		}
	}

	auto FVulkanDescriptorPool::MarkUsed(FVulkanCompletionToken Token) -> void
	{
		check(Token > 0 && NumAllocatedDescriptorSets > 0);
		LastUseToken = std::max(LastUseToken, Token);
	}

	auto FVulkanDescriptorPool::Reset(
		FVulkanCompletionToken CompletedToken) -> void
	{
		check(LastUseToken <= CompletedToken);
		Device->GetHandle().resetDescriptorPool(DescriptorPool);
		GVulkanMemoryBaselineTracker.RecordDescriptorPoolReset(
			NumAllocatedDescriptorSets);
		NumAllocatedDescriptorSets = 0;
		NumAllocatedDescriptors.clear();
		LastUseToken = 0;
	}

	auto FVulkanGlobalDescriptorPool::GetActiveBatch() -> FPoolBatch&
	{
		CheckVulkanRHIThread();
		check(ActiveBatchIndex < Batches.size());
		return Batches[ActiveBatchIndex];
	}

	auto FVulkanGlobalDescriptorPool::CreatePool(
		const FVulkanDescriptorRequirements& Requirements,
		uint32 TargetMaxSets) -> FVulkanDescriptorPool&
	{
		constexpr uint32 MaxPoolsPerBatch = 32;
		const FVulkanDescriptorRequirements PoolRequirements =
			Requirements.ScaleToSetCapacity(TargetMaxSets);

		FPoolBatch& Batch = GetActiveBatch();
		auto& Pools = Batch.Pools;
		checkf(Pools.size() < MaxPoolsPerBatch,
			"Vulkan descriptor pool expansion limit reached: batch={}, poolCount={}, requestedSets={}",
			ActiveBatchIndex, Pools.size(), Requirements.MaxSets);
		const bool bIsExpansion = !Pools.empty();
		Pools.push_back(std::make_unique<FVulkanDescriptorPool>(&Device, PoolRequirements));
		if (bIsExpansion)
		{
			++Batch.ExpansionCount;
			++Device.GetPipelineCacheStatisticsMutable().DescriptorPoolExpansions;
		}
		DURIN_DEBUG("Created Vulkan descriptor pool: batch={}, poolCount={}, maxSets={}, expansions={}",
			ActiveBatchIndex, Pools.size(), Pools.back()->GetMaxSets(),
			Batch.ExpansionCount);
		return *Pools.back();
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
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max())
		{
			PrepareForUse();
		}

		auto& Pools = GetActiveBatch().Pools;
		for (const auto& Pool : Pools)
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

		uint32 TargetMaxSets = kInitialDescriptorPoolSetCapacity;
		if (!Pools.empty())
		{
			TargetMaxSets = GetNextDescriptorPoolSetCapacity(
				Pools.back()->GetMaxSets());
		}
		FVulkanDescriptorPool& NewPool = CreatePool(Requirements, TargetMaxSets);
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

	auto FVulkanGlobalDescriptorPool::PrepareForUse() -> void
	{
		CheckVulkanRHIThread();
		check(ActiveBatchIndex == std::numeric_limits<uint32>::max());
		auto& Tracker = Device.GetCompletionTracker();
		Tracker.Poll();
		FVulkanCompletionToken Completed = Tracker.GetCompletedToken();
		for (uint32 Index = 0; Index < Batches.size(); ++Index)
		{
			if (Batches[Index].LastUseToken <= Completed)
			{
				ActiveBatchIndex = Index;
				break;
			}
		}
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max()
			&& Batches.size() < kFrameInFlight)
		{
			ActiveBatchIndex = static_cast<uint32>(Batches.size());
			Batches.emplace_back();
		}
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max())
		{
			const auto Oldest = std::ranges::min_element(
				Batches, {}, &FPoolBatch::LastUseToken);
			check(Oldest != Batches.end() && Oldest->LastUseToken > 0);
			Tracker.WaitForToken(Oldest->LastUseToken);
			Completed = Tracker.GetCompletedToken();
			ActiveBatchIndex = static_cast<uint32>(
				std::distance(Batches.begin(), Oldest));
		}
		FPoolBatch& Batch = GetActiveBatch();
		check(Batch.LastUseToken <= Completed);
		for (const auto& Pool : Batch.Pools)
		{
			Pool->Reset(Completed);
		}
		Batch.LastUseToken = 0;
	}

	auto FVulkanGlobalDescriptorPool::RetireUsedPools(
		FVulkanCompletionToken Token) -> void
	{
		CheckVulkanRHIThread();
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max())
		{
			return;
		}
		FPoolBatch& Batch = GetActiveBatch();
		bool bUsed = false;
		for (const auto& Pool : Batch.Pools)
		{
			if (Pool->GetAllocatedSets() > 0)
			{
				Pool->MarkUsed(Token);
				bUsed = true;
			}
		}
		Batch.LastUseToken = bUsed ? Token : 0;
		ActiveBatchIndex = std::numeric_limits<uint32>::max();
	}

	auto FVulkanGlobalDescriptorPool::GetBatchTokensForTesting() const
		-> std::array<FVulkanCompletionToken, kFrameInFlight>
	{
		std::array<FVulkanCompletionToken, kFrameInFlight> Result{};
		for (uint32 Index = 0; Index < Batches.size(); ++Index)
		{
			Result[Index] = Batches[Index].LastUseToken;
		}
		return Result;
	}
} // namespace Durin::VulkanRHI
