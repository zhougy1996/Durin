#include "VulkanDeferredBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanView.h"
#include "VulkanDiagnostics.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanBindingAdmission::FReservation::~FReservation()
	{
		if (Owner) Owner->Release(Slots);
	}

	auto FVulkanBindingAdmission::Release(const std::vector<FSlot>& Slots) -> void
	{
		std::lock_guard Lock(Mutex);
		for (const auto& Slot : Slots)
		{
			Pages[Slot.Page].Used.erase(Slot.Offset);
			FRHIDeferredBufferBackend::RecordAdmission(Slot.Size, 0, false);
		}
	}

	FVulkanBindingAdmission::~FVulkanBindingAdmission()
	{
		FRHIDeferredBufferBackend::RecordAdmission(0, Capacity, false);
	}

	auto FVulkanBindingAdmission::Reserve(uint64 Size) -> std::shared_ptr<FReservation>
	{
		require(Size != 0 && Size <= UINT32_MAX - (Alignment - 1));
		Size = (Size + Alignment - 1) / Alignment * Alignment;
		auto Result = std::make_shared<FReservation>();
		Result->Owner = shared_from_this();
		Result->Slots.reserve(Queues.size());
		std::lock_guard Lock(Mutex);
		for (const auto Queue : Queues)
		{
			bool bFound = false;
			for (uint32 Index = 0; Index < Pages.size(); ++Index)
			{
				auto& Page = Pages[Index];
				if (Page.Queue != Queue || Page.Size < Size) continue;
				uint64 Offset = 0;
				for (const auto& [Start, Length] : Page.Used)
				{
					if (Size <= Start - Offset) break;
					Offset = Start + Length;
				}
				if (Size > Page.Size - Offset) continue;
				Page.Used.emplace(Offset, Size);
				Result->Slots.push_back({Index, Page.Size, Queue, Offset, Size});
				FRHIDeferredBufferBackend::RecordAdmission(Size, 0, true);
				bFound = true;
				break;
			}
			if (bFound) continue;
			const uint64 PageSize = std::max<uint64>(4ull * 1024 * 1024, Size);
			Pages.push_back({PageSize, Queue, {{0, Size}}});
			Capacity += PageSize;
			Result->Slots.push_back({static_cast<uint32>(Pages.size() - 1), PageSize, Queue, 0, Size});
			FRHIDeferredBufferBackend::RecordAdmission(Size, PageSize, true);
		}
		return Result;
	}

	auto TestVulkanBindingAdmission() -> bool
	{
		constexpr uint64 PageSize = 4ull * 1024 * 1024;
		const auto& QueueInfos = FVulkanDynamicRHI::Get().GetDeviceForTesting()->GetQueueCapabilities().Queues;
		std::vector<FRHIQueueId> Queues;
		for (const auto& Queue : QueueInfos) Queues.push_back(Queue.Id);
		auto Admission = std::make_shared<FVulkanBindingAdmission>(256, Queues);
		auto A = Admission->Reserve(1), B = Admission->Reserve(1), Tail = Admission->Reserve(PageSize - 512);
		if (!A || !B || !Tail || A->Slots.size() != Queues.size()) return false;
		for (size_t Index = 0; Index < Queues.size(); ++Index)
			if (A->Slots[Index].Queue != Queues[Index] || A->Slots[Index].Offset != 0
				|| B->Slots[Index].Offset != 256 || Tail->Slots[Index].Offset != 512) return false;
		// Fragmented pages grow without waiting; released adjacent intervals are reused.
		B.reset();
		auto Grown = Admission->Reserve(512);
		if (!Grown || Grown->Slots.front().Page == A->Slots.front().Page) return false;
		A.reset();
		auto Reused = Admission->Reserve(512);
		if (!Reused || Reused->Slots.front().Offset != 0) return false;
		Reused.reset(); Tail.reset();
		std::vector<std::shared_ptr<FVulkanBindingAdmission::FReservation>> Concurrent(16);
		std::vector<std::thread> Threads;
		for (size_t Index = 0; Index < Concurrent.size(); ++Index)
			Threads.emplace_back([&, Index] { Concurrent[Index] = Admission->Reserve(PageSize / 8); });
		for (auto& Thread : Threads) Thread.join();
		return std::ranges::count_if(Concurrent, [](const auto& Item) { return bool(Item); }) == 16;
	}

	FVulkanBindingPool::FVulkanBindingPool(FVulkanDevice& InDevice, bool bInUniform)
		: Device(InDevice), bUniform(bInUniform)
	{
		const auto& Limits = Device.GetGpuProperties().limits;
		std::vector<FRHIQueueId> Queues;
		for (const auto& Queue : Device.GetQueueCapabilities().Queues) Queues.push_back(Queue.Id);
		// Uniform pages are immutable and concurrently shared; one interval serves every queue.
		if (bUniform) Queues.resize(1);
		Admission = std::make_shared<FVulkanBindingAdmission>(std::max<uint64>({16, Limits.nonCoherentAtomSize,
			bUniform ? Limits.minUniformBufferOffsetAlignment : Limits.minStorageBufferOffsetAlignment}),
			std::move(Queues));
	}

	FVulkanBindingPool::~FVulkanBindingPool()
	{
		CheckVulkanRHIThread();
		for (const auto& [Index, Buffer] : Buffers)
			if (Buffer) GVulkanMemoryBaselineTracker.RecordArenaPageFreed(EVulkanAllocationClassCandidate::DynamicUpload, Buffer->GetSize());
	}

	auto FVulkanBindingPool::Resolve(const std::shared_ptr<void>& Reservation, FRHIQueueId Queue)
		-> std::pair<TRefCountPtr<FVulkanBuffer>, uint64>
	{
		CheckVulkanRHIThread();
		const auto Ticket = std::static_pointer_cast<FVulkanBindingAdmission::FReservation>(Reservation);
		require(Ticket && Ticket->Owner == Admission);
		const auto Slot = bUniform ? Ticket->Slots.begin()
			: std::ranges::find(Ticket->Slots, Queue, &FVulkanBindingAdmission::FSlot::Queue);
		require(Slot != Ticket->Slots.end());
		auto& Buffer = Buffers[Slot->Page];
		if (!Buffer)
		{
			const auto Usage = EBufferUsageFlags::Dynamic | (bUniform ? EBufferUsageFlags::UniformBuffer
				: EBufferUsageFlags::ShaderResource | EBufferUsageFlags::ByteAddressBuffer);
			Buffer = new FVulkanBuffer(Device, FRHIBufferCreateDesc::Create("AdmittedBindingPage",
				static_cast<uint32>(Slot->PageSize), 0, Usage), bUniform);
			GVulkanMemoryBaselineTracker.RecordArenaPageAllocated(EVulkanAllocationClassCandidate::DynamicUpload, Slot->PageSize);
		}
		return {Buffer, Slot->Offset};
	}

	auto FVulkanDynamicRHI::RHIReserveBufferBacking(const FRHIBufferDesc& Desc)
		-> std::shared_ptr<void>
	{
		return Device->GetBindingPool(EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)).Reserve(Desc.Size);
	}

	namespace
	{
		struct FDeferredBacking
		{
			TRefCountPtr<FVulkanBuffer> Buffer;
			std::shared_ptr<void> Lease;
			uint64 Offset = 0;
			uint64 Size = 0;
			~FDeferredBacking()
			{
				if (Size) GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(EVulkanAllocationClassCandidate::DynamicUpload, Size);
			}
		};
	}

	auto FVulkanShaderBufferBindings::Update(
		std::span<const FRHIShaderParameterResource> Parameters) -> void
	{
		for (const auto& Parameter : Parameters)
		{
			const auto It = std::ranges::find_if(Bindings, [&](const FBinding& Binding) {
				return Binding.Parameter.SetIndex == Parameter.SetIndex
					&& Binding.Parameter.BindingIndex == Parameter.BindingIndex
					&& Binding.Parameter.ArrayElement == Parameter.ArrayElement;
			});
			if (IsLogicalBufferBindingResource(Parameter.Resource))
			{
				if (It != Bindings.end() && It->Logical.GetReference() == Parameter.Resource)
				{
					It->Parameter = Parameter;
					It->bDirty = true;
					continue;
				}
				FBinding Binding{Parameter, Parameter.Resource, {}};
				if (It == Bindings.end()) Bindings.push_back(std::move(Binding));
				else *It = std::move(Binding);
			}
			else if (It != Bindings.end()) Bindings.erase(It);
		}
	}

	auto FVulkanShaderBufferBindings::Resolve(FVulkanDevice& Device,
		FVulkanCommandListContext& Context) -> std::vector<FRHIShaderParameterResource>
	{
		std::vector<FRHIShaderParameterResource> Result;
		Result.reserve(Bindings.size());
		for (auto& Binding : Bindings)
		{
			if (!Binding.bDirty) continue;
			const bool bUniformResource = Binding.Logical->GetResourceType() == ERHIResourceType::UniformBuffer;
			const bool bDynamic = Binding.Parameter.Type == ERHIBindingType::UniformBufferDynamic;
			std::shared_ptr<const FRHIStorageBufferSnapshot> Snapshot;
			std::shared_ptr<void> Backing, Lease;
			TRefCountPtr<FVulkanBuffer> Buffer;
			uint64 AllocationOffset = 0;
			FRHIBufferViewDesc ViewDesc;
			if (bUniformResource)
			{
				const auto& Allocation = static_cast<FVulkanUniformBuffer*>(Binding.Logical.GetReference())->GetAllocation();
				Backing = Allocation;
				Lease = Allocation->Lease;
				Buffer = Allocation->Buffer;
				AllocationOffset = Allocation->Offset;
				ViewDesc = {bDynamic ? 0 : Binding.Parameter.Offset, Binding.Parameter.Size, ERHIBufferViewType::Uniform};
			}
			else
			{
				auto* Logical = static_cast<FRHIBufferView*>(Binding.Logical.GetReference());
				Snapshot = FRHIDeferredBufferBackend::ResolveSnapshot(*Logical->GetBuffer());
				const auto& Desc = Logical->GetBuffer()->GetDesc();
				auto Storage = std::static_pointer_cast<FDeferredBacking>(
					FRHIDeferredBufferBackend::GetBacking(*Snapshot, Context.GetQueue()));
				if (!Storage)
				{
					const auto [Page, Offset] = Device.GetBindingPool(false).Resolve(
						Snapshot->GetBackingAdmission(), Context.GetQueue()->GetId());
					Storage = std::make_shared<FDeferredBacking>();
					Storage->Buffer = Page;
					Storage->Offset = Offset;
					Storage->Lease = Snapshot->GetBackingAdmission();
					Page->InitializeDeferredReadOnly(Snapshot->GetData(), static_cast<uint32>(Offset));
					const auto& Limits = Device.GetGpuProperties().limits;
					const uint64 Alignment = std::max<uint64>({16, Limits.nonCoherentAtomSize, Limits.minStorageBufferOffsetAlignment});
					Storage->Size = (Desc.Size + Alignment - 1) / Alignment * Alignment;
					GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(
						EVulkanAllocationClassCandidate::DynamicUpload, Storage->Size, false, false);
					FRHIDeferredBufferBackend::SetBacking(*Snapshot, Context.GetQueue(), Storage);
				}
				Backing = Storage;
				Lease = Storage->Lease;
				Buffer = Storage->Buffer;
				AllocationOffset = Storage->Offset;
				ViewDesc = Logical->GetDesc();
			}
			ViewDesc.Offset += AllocationOffset;
			const uint64 PhysicalOffset = ViewDesc.Offset;
			if (bDynamic) ViewDesc.Offset = 0;
			auto Resolved = Binding.Resolved;
			if (!Resolved || Resolved->Backing != Backing || Resolved->bDynamic != bDynamic
				|| Resolved->PhysicalOffset != PhysicalOffset || Resolved->View->GetDesc() != ViewDesc)
			{
				Resolved = std::make_shared<FResolved>();
				Resolved->Snapshot = std::move(Snapshot);
				Resolved->Backing = std::move(Backing);
				Resolved->Lease = std::move(Lease);
				Resolved->PhysicalOffset = PhysicalOffset;
				Resolved->bDynamic = bDynamic;
				Resolved->View = bDynamic
					? Device.GetRHI().RHIGetOrCreateBufferView(Buffer, ViewDesc)
					: TRefCountPtr<FRHIBufferView>(new FVulkanBufferView(Device, Buffer, ViewDesc));
				requiref(Resolved->View, "Could not create a shader buffer descriptor view.");
				Binding.Resolved = Resolved;
			}
			Binding.bDirty = false;
			auto Parameter = Binding.Parameter;
			Parameter.Resource = Resolved->View.GetReference();
			if (bDynamic)
			{
				const uint64 Offset = Resolved->PhysicalOffset + Parameter.Offset;
				requiref(Offset <= UINT32_MAX, "Deferred dynamic uniform offset exceeds Vulkan limits.");
				Parameter.Offset = static_cast<uint32>(Offset);
			}
			Result.push_back(Parameter);
		}
		return Result;
	}

	auto FVulkanShaderBufferBindings::PrepareForUse(
		FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void
	{
		for (const auto& Binding : Bindings)
		{
			const auto& Resolved = Binding.Resolved;
			check(Resolved && !Binding.bDirty);
			// A new submission must retain the captured allocation even without a rebind.
			auto* Buffer = FVulkanBuffer::Cast(Resolved->View->GetBuffer());
			const bool bUniform = Binding.Parameter.Type != ERHIBindingType::StorageBuffer;
			// Read-to-read changes need no memory dependency for immutable host-initialized data.
			const uint64 Offset = Resolved->PhysicalOffset
				+ (Resolved->bDynamic ? Binding.Parameter.Offset : 0);
			const ERHIAccess Expected = Pipeline == ERHIPipeline::Compute
				? (bUniform ? ERHIAccess::ComputeUniformRead : ERHIAccess::ComputeShaderRead)
				: (bUniform ? ERHIAccess::GraphicsUniformRead : ERHIAccess::GraphicsShaderRead);
			ERHIAccess Tracked = ERHIAccess::None;
			if (!Buffer->GetStateTracker().Validate(Offset, Resolved->View->GetDesc().Size, Expected, Tracked))
				Buffer->GetStateTracker().Apply(Offset, Resolved->View->GetDesc().Size, Expected);
			Context.RetainAllocation(Resolved->Lease);
			Context.RetainAllocation(Resolved);
		}
	}
}
