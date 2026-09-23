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
		if (!Size || Size > 16ull * 1024 * 1024) return {};
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
			if (PageSize > MaxCapacity - Capacity) return {};
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
		auto Admission = std::make_shared<FVulkanBindingAdmission>(256, PageSize * Queues.size(), Queues);
		auto A = Admission->Reserve(1), B = Admission->Reserve(1), Tail = Admission->Reserve(PageSize - 512);
		if (!A || !B || !Tail || A->Slots.size() != Queues.size() || Admission->Reserve(1)) return false;
		for (size_t Index = 0; Index < Queues.size(); ++Index)
			if (A->Slots[Index].Queue != Queues[Index] || A->Slots[Index].Offset != 0
				|| B->Slots[Index].Offset != 256 || Tail->Slots[Index].Offset != 512) return false;
		// Fragmentation rejects before recording; releasing both adjacent intervals
		// makes a larger contiguous reservation possible without native work.
		B.reset();
		if (Admission->Reserve(512)) return false;
		A.reset();
		auto Reused = Admission->Reserve(512);
		if (!Reused || Reused->Slots.front().Offset != 0) return false;
		Reused.reset(); Tail.reset();
		std::vector<std::shared_ptr<FVulkanBindingAdmission::FReservation>> Concurrent(16);
		std::vector<std::thread> Threads;
		for (size_t Index = 0; Index < Concurrent.size(); ++Index)
			Threads.emplace_back([&, Index] { Concurrent[Index] = Admission->Reserve(PageSize / 8); });
		for (auto& Thread : Threads) Thread.join();
		return std::ranges::count_if(Concurrent, [](const auto& Item) { return bool(Item); }) == 8;
	}

	FVulkanBindingPool::FVulkanBindingPool(FVulkanDevice& InDevice, bool bInUniform)
		: Device(InDevice), bUniform(bInUniform)
	{
		const auto& Limits = Device.GetGpuProperties().limits;
		std::vector<FRHIQueueId> Queues;
		for (const auto& Queue : Device.GetQueueCapabilities().Queues) Queues.push_back(Queue.Id);
		Admission = std::make_shared<FVulkanBindingAdmission>(std::max<uint64>({16, Limits.nonCoherentAtomSize,
			bUniform ? Limits.minUniformBufferOffsetAlignment : Limits.minStorageBufferOffsetAlignment}),
			(bUniform ? 64ull : 128ull) * 1024 * 1024, std::move(Queues));
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
		const auto Slot = std::ranges::find(Ticket->Slots, Queue, &FVulkanBindingAdmission::FSlot::Queue);
		require(Slot != Ticket->Slots.end());
		auto& Buffer = Buffers[Slot->Page];
		if (!Buffer)
		{
			const auto Usage = EBufferUsageFlags::Dynamic | (bUniform ? EBufferUsageFlags::UniformBuffer
				: EBufferUsageFlags::ShaderResource | EBufferUsageFlags::ByteAddressBuffer);
			Buffer = new FVulkanBuffer(Device, FRHIBufferCreateDesc::Create("AdmittedBindingPage",
				static_cast<uint32>(Slot->PageSize), 0, Usage));
			GVulkanMemoryBaselineTracker.RecordArenaPageAllocated(EVulkanAllocationClassCandidate::DynamicUpload, Slot->PageSize);
		}
		return {Buffer, Slot->Offset};
	}

	auto FVulkanDynamicRHI::RHIReserveBufferBacking(const FRHIBufferDesc& Desc)
		-> std::expected<std::shared_ptr<void>, ERHIBufferUploadError>
	{
		auto Reservation = Device->GetBindingPool(EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)).Reserve(Desc.Size);
		if (!Reservation) return std::unexpected(ERHIBufferUploadError::PayloadBudgetExceeded);
		return Reservation;
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

	auto FVulkanDeferredBufferBindings::Update(
		std::span<const FRHIShaderParameterResource> Parameters) -> void
	{
		for (const auto& Parameter : Parameters)
		{
			const auto It = std::ranges::find_if(Bindings, [&](const FBinding& Binding) {
				return Binding.Parameter.SetIndex == Parameter.SetIndex
					&& Binding.Parameter.BindingIndex == Parameter.BindingIndex
					&& Binding.Parameter.ArrayElement == Parameter.ArrayElement;
			});
			if (IsCPUAuthoredBufferResource(Parameter.Resource))
			{
				if (It != Bindings.end() && It->Logical.GetReference() == Parameter.Resource)
				{
					It->Parameter = Parameter;
					continue;
				}
				FBinding Binding{Parameter, static_cast<FRHIBufferView*>(Parameter.Resource), {}};
				if (It == Bindings.end()) Bindings.push_back(std::move(Binding));
				else *It = std::move(Binding);
			}
			else if (It != Bindings.end()) Bindings.erase(It);
		}
	}

	auto FVulkanDeferredBufferBindings::Resolve(FVulkanDevice& Device,
		FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> std::vector<FRHIShaderParameterResource>
	{
		std::vector<FRHIShaderParameterResource> Result;
		Result.reserve(Bindings.size());
		for (auto& Binding : Bindings)
		{
			const auto Snapshot = FRHIDeferredBufferBackend::ResolveSnapshot(*Binding.Logical->GetBuffer());
			auto Resolved = Binding.Resolved.lock();
			if (!Resolved || Resolved->Snapshot != Snapshot)
			{
				Resolved.reset();
				const auto& Desc = Binding.Logical->GetBuffer()->GetDesc();
				auto Backing = std::static_pointer_cast<FDeferredBacking>(
					FRHIDeferredBufferBackend::GetBacking(*Snapshot, Context.GetQueue()));
				if (!Backing)
				{
					const auto [Buffer, Offset] = Device.GetBindingPool(
						EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)).Resolve(
						Snapshot->GetBackingAdmission(), Context.GetQueue()->GetId());
					Backing = std::make_shared<FDeferredBacking>();
					Backing->Buffer = Buffer;
					Backing->Offset = Offset;
					Backing->Lease = Snapshot->GetBackingAdmission();
					Backing->Buffer->InitializeDeferredReadOnly(Snapshot->GetData(), static_cast<uint32>(Offset));
					const auto& Limits = Device.GetGpuProperties().limits;
					const uint64 Alignment = std::max<uint64>({16, Limits.nonCoherentAtomSize,
						EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)
							? Limits.minUniformBufferOffsetAlignment : Limits.minStorageBufferOffsetAlignment});
					Backing->Size = (Desc.Size + Alignment - 1) / Alignment * Alignment;
					GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(EVulkanAllocationClassCandidate::DynamicUpload,
						Backing->Size, false, false);
					FRHIDeferredBufferBackend::SetBacking(*Snapshot, Context.GetQueue(), Backing);
				}
				auto ViewDesc = Binding.Logical->GetDesc();
				ViewDesc.Offset += Backing->Offset;
				const auto& Limits = Device.GetGpuProperties().limits;
				const bool bUniform = ViewDesc.Type == ERHIBufferViewType::Uniform;
				const uint64 Alignment = bUniform ? Limits.minUniformBufferOffsetAlignment
					: Limits.minStorageBufferOffsetAlignment;
				const uint64 MaxRange = bUniform ? Limits.maxUniformBufferRange : Limits.maxStorageBufferRange;
				if ((Alignment && ViewDesc.Offset % Alignment != 0) || ViewDesc.Size > MaxRange)
					throw std::runtime_error("Deferred buffer view exceeds native binding limits.");
				Resolved = std::make_shared<FResolved>();
				Resolved->Snapshot = Snapshot;
				Resolved->Backing = Backing;
				Resolved->Lease = Backing->Lease;
				Resolved->View = new FVulkanBufferView(Device, Backing->Buffer, ViewDesc);
				Binding.Resolved = Resolved;
			}
			// Each queue submission owns the exact version, even after a later update/rebind.
			auto* Buffer = FVulkanBuffer::Cast(Resolved->View->GetBuffer());
			const bool bUniform = Binding.Logical->GetDesc().Type == ERHIBufferViewType::Uniform;
			// Read-to-read changes need no memory dependency for immutable host-initialized data.
			Buffer->GetStateTracker().Apply(Resolved->View->GetDesc().Offset, Resolved->View->GetDesc().Size, Pipeline == ERHIPipeline::Compute
				? (bUniform ? ERHIAccess::ComputeUniformRead : ERHIAccess::ComputeShaderRead)
				: (bUniform ? ERHIAccess::GraphicsUniformRead : ERHIAccess::GraphicsShaderRead));
			Context.RetainAllocation(Resolved->Lease);
			Context.RetainAllocation(Resolved);
			auto Parameter = Binding.Parameter;
			Parameter.Resource = Resolved->View.GetReference();
			Result.push_back(Parameter);
		}
		return Result;
	}
}
