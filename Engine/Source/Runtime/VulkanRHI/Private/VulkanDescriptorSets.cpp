#include "VulkanDescriptorSets.h"

#include "VulkanDevice.h"
#include "VulkanSubmission.h"
#include "VulkanCompletion.h"
#include "VulkanQueue.h"
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
		if (DLayoutMap.size() >= 4096)
			throw FRHIRecoverableCreationError("Vulkan descriptor-set layout cache is full.");
		// Resident entries remain valid until all creation tasks have joined.
		vk::DescriptorSetLayoutCreateInfo CreateInfo{};
		CreateInfo.setBindings(Layout.LayoutBindings);

		FVulkanDescriptorSetLayoutEntry NewEntry;
		NewEntry.MetadataReservation = Device.ReserveCacheMetadata(512 + Layout.LayoutBindings.capacity() * sizeof(vk::DescriptorSetLayoutBinding));
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::DescriptorSetLayout);
#endif
		NewEntry.Handle = Device.GetHandle().createDescriptorSetLayout(CreateInfo);
		try
		{
			Device.GetRHI().GetDebugUtils().NameObject(NewEntry.Handle,
				Device.GetRHI().GetDebugUtils().MakeInternalName("DescriptorSetLayout"));
			NewEntry.HandleId = ++GVulkanDSetLayoutHandleIdCounter;
			const auto [InsertedIt, bInserted] = DLayoutMap.emplace(Layout, NewEntry);
			check(bInserted);
		}
		catch (...)
		{
			Device.GetHandle().destroyDescriptorSetLayout(NewEntry.Handle);
			throw;
		}
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanDescriptorSetLayoutEntryCount.fetch_add(1, std::memory_order_release);
#endif

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

	auto FVulkanDescriptorPool::Reset() -> void
	{
		Device->GetHandle().resetDescriptorPool(DescriptorPool);
		GVulkanMemoryBaselineTracker.RecordDescriptorPoolReset(
			NumAllocatedDescriptorSets);
		NumAllocatedDescriptorSets = 0;
		NumAllocatedDescriptors.clear();
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
			++Device.AccessPipelineCacheStatistics().Get().DescriptorPoolExpansions;
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

		uint32 TargetMaxSets = InitialDescriptorPoolSetCapacity;
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
		Device.PollQueues();
		for (uint32 Index = 0; Index < Batches.size(); ++Index)
		{
			if (Batches[Index].Owner.expired())
			{
				ActiveBatchIndex = Index;
				break;
			}
		}
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max()
			&& Batches.size() < FrameInFlight)
		{
			ActiveBatchIndex = static_cast<uint32>(Batches.size());
			Batches.emplace_back();
		}
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max())
		{
			const auto Oldest = std::ranges::min_element(
				Batches, {}, &FPoolBatch::RetirementOrder);
			check(Oldest != Batches.end());
			Device.GetSubmissionCoordinator().WaitForAllocation(Oldest->Owner);
			ActiveBatchIndex = static_cast<uint32>(
				std::distance(Batches.begin(), Oldest));
		}
		FPoolBatch& Batch = GetActiveBatch();
		require(Batch.Owner.expired());
		auto Owner = std::make_shared<FAllocationLease>();
		for (const auto& Pool : Batch.Pools)
		{
			Pool->Reset();
		}
		ActiveOwner = std::move(Owner);
		Batch.Owner = ActiveOwner;
	}

	auto FVulkanGlobalDescriptorPool::RetireUsedPools() -> void
	{
		CheckVulkanRHIThread();
		if (ActiveBatchIndex == std::numeric_limits<uint32>::max())
		{
			return;
		}
		FPoolBatch& Batch = GetActiveBatch();
		Batch.RetirementOrder = NextRetirementOrder++;
		ActiveOwner.reset();
		ActiveBatchIndex = std::numeric_limits<uint32>::max();
	}

	auto FVulkanGlobalDescriptorPool::GetBatchTokensForTesting() const
		-> std::array<FVulkanCompletionToken, FrameInFlight>
	{
		std::array<FVulkanCompletionToken, FrameInFlight> Result{};
		for (uint32 Index = 0; Index < Batches.size(); ++Index)
		{
			// Compatibility diagnostics project graphics only; never used for reuse.
			if (const auto Owner = Batches[Index].Owner.lock())
			{
				const auto Uses = Device.GetSubmissionCoordinator().GetAllocationUses(Owner);
				for (const auto& Ticket : Uses.GetTickets())
					if (Ticket.GetPoint().Queue == Device.GetGraphicsQueue()->GetId())
						Result[Index] = Ticket.GetPoint().Value;
			}
		}
		return Result;
	}
} // namespace Durin::VulkanRHI
