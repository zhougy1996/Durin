#include "Backend/RHICompletionBackend.h"
#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "Profiling/Profiling.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanContext.h"
#include "VulkanDynamicBufferPool.h"
#include "VulkanQueue.h"

namespace Durin::VulkanRHI
{
	namespace
	{
#if DURIN_WITH_TRACY
		struct FDynamicBufferWriteProfile final
		{
			std::atomic<uint64> MappedLocks = 0, PoolBusyFallbacks = 0, NoPoolFallbacks = 0;
			std::atomic<uint64> RequestedBytes = 0, CommittedMappedBytes = 0;
			std::atomic<uint64> MappedReservedBytes = 0, FallbackReservedBytes = 0, LiveMappedPages = 0;
		};
		FDynamicBufferWriteProfile GDynamicBufferWriteProfile;

		auto PublishDynamicBufferWriteProfile() -> void
		{
			if (!TracyIsConnected) return;
			const auto Mapped = GDynamicBufferWriteProfile.MappedLocks.load(std::memory_order_relaxed);
			const auto Busy = GDynamicBufferWriteProfile.PoolBusyFallbacks.load(std::memory_order_relaxed);
			const auto NoPool = GDynamicBufferWriteProfile.NoPoolFallbacks.load(std::memory_order_relaxed);
			TracyPlot("Vulkan.DynamicBuffer.MappedLocks", static_cast<int64>(Mapped));
			TracyPlot("Vulkan.DynamicBuffer.PoolBusyFallbacks", static_cast<int64>(Busy));
			TracyPlot("Vulkan.DynamicBuffer.NoPoolFallbacks", static_cast<int64>(NoPool));
			TracyPlot("Vulkan.DynamicBuffer.MappingHitPercent", Mapped + Busy + NoPool
				? 100.0 * static_cast<double>(Mapped) / static_cast<double>(Mapped + Busy + NoPool) : 0.0);
			TracyPlot("Vulkan.DynamicBuffer.RequestedBytes",
				static_cast<int64>(GDynamicBufferWriteProfile.RequestedBytes.load(std::memory_order_relaxed)));
			TracyPlot("Vulkan.DynamicBuffer.CommittedMappedBytes",
				static_cast<int64>(GDynamicBufferWriteProfile.CommittedMappedBytes.load(std::memory_order_relaxed)));
			TracyPlot("Vulkan.DynamicBuffer.MappedReservedBytes",
				static_cast<int64>(GDynamicBufferWriteProfile.MappedReservedBytes.load(std::memory_order_relaxed)));
			TracyPlot("Vulkan.DynamicBuffer.FallbackReservedBytes",
				static_cast<int64>(GDynamicBufferWriteProfile.FallbackReservedBytes.load(std::memory_order_relaxed)));
			TracyPlot("Vulkan.DynamicBuffer.LiveMappedPages",
				static_cast<int64>(GDynamicBufferWriteProfile.LiveMappedPages.load(std::memory_order_relaxed)));
		}
#endif

		auto GetCanonicalBufferAccess(EBufferUsageFlags Usage) -> ERHIAccess
		{
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UnorderedAccess))
				return ERHIAccess::GraphicsShaderReadWrite;
			ERHIAccess Access = ERHIAccess::None;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::VertexBuffer)) Access |= ERHIAccess::VertexBufferRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::IndexBuffer)) Access |= ERHIAccess::IndexBufferRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer)) Access |= ERHIAccess::GraphicsUniformRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer))
				Access |= ERHIAccess::GraphicsShaderRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::SourceCopy)) Access |= ERHIAccess::TransferRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::DrawIndirect)) Access |= ERHIAccess::IndirectArgumentRead;
			return Access;
		}
	}

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc,
		bool bConcurrentReadOnly, vk::Result* OutResult, FVulkanBuffer* AliasSource)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
		, StateTracker(InCreateDesc.Size)
		, DebugName(Device.GetRHI().GetDebugUtils().CanNameObjects()
			? (InCreateDesc.DebugName ? std::string(InCreateDesc.DebugName)
				: Device.GetRHI().GetDebugUtils().MakeInternalName("Buffer")) : std::string{})
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InCreateDesc.Size);
		BufferInfo.setUsage(ToVulkan_BufferUsageFlags(InCreateDesc.Usage));
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);
		std::vector<uint32> QueueFamilies;
		if (bConcurrentReadOnly)
		{
			for (const auto& Queue : Device.GetQueueCapabilities().Queues)
				if (std::ranges::find(QueueFamilies, Queue.OwnershipDomain) == QueueFamilies.end())
					QueueFamilies.push_back(Queue.OwnershipDomain);
			if (QueueFamilies.size() > 1)
				BufferInfo.setSharingMode(vk::SharingMode::eConcurrent).setQueueFamilyIndices(QueueFamilies);
		}

		EVulkanAllocationClassCandidate AllocationCandidate =
			EVulkanAllocationClassCandidate::DeviceLocal;
		if (EnumHasAnyFlags(InCreateDesc.Usage, EBufferUsageFlags::Dynamic))
		{
			AllocationCandidate = EVulkanAllocationClassCandidate::DynamicUpload;
			if (EnumHasAllFlags(InCreateDesc.Usage,
				EBufferUsageFlags::DestinationCopy | EBufferUsageFlags::KeepCPUAccessible))
			{
				AllocationCandidate = EVulkanAllocationClassCandidate::TransferReadback;
			}
			else if (EnumHasAnyFlags(InCreateDesc.Usage, EBufferUsageFlags::SourceCopy)
				&& !EnumHasAnyFlags(InCreateDesc.Usage,
					EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::VertexBuffer
						| EBufferUsageFlags::IndexBuffer))
			{
				AllocationCandidate = EVulkanAllocationClassCandidate::TransferUpload;
			}
		}

		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		bAllowAliasing = InCreateDesc.Aliasing == ERHITransientAliasing::Enabled || AliasSource != nullptr;
		vk::Result Result;
		if (AliasSource)
		{
			require(AliasSource->bAllowAliasing && !bConcurrentReadOnly
				&& AllocationCandidate == EVulkanAllocationClassCandidate::DeviceLocal && &AliasSource->Device == &Device);
			AliasOwner = AliasSource->AliasOwner ? AliasSource->AliasOwner.GetReference() : AliasSource;
			Allocation = AliasSource->Allocation;
			Result = MemoryManager.CreateAliasingBuffer(Allocation, BufferInfo, Buffer);
		}
		else Result = MemoryManager.CreateBuffer(Allocation, Buffer, AllocationCandidate,
			BufferInfo, DebugName.c_str(), bAllowAliasing);
		if (OutResult) *OutResult = Result;
		if (Result != vk::Result::eSuccess)
		{
			Buffer = nullptr;
			Allocation = {};
			if (!OutResult) ThrowVulkanError(Result, "Vulkan buffer allocation");
			return;
		}
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Buffer, [&] { return std::string_view(DebugName); });
	}

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InDesc,
		TRefCountPtr<FVulkanBuffer> Page, uint32 Offset, uint32 MappedSize)
		: FRHIBuffer(InDesc), Device(InDevice), StateTracker(InDesc.Size),
		PageBuffer(std::move(Page)), PageOffset(Offset), PageSize(MappedSize)
	{
		require(PageBuffer && MappedSize && MappedSize <= InDesc.Size && uint64(Offset) + MappedSize <= PageBuffer->GetSize());
	}

	auto FVulkanBuffer::MarkMappedPage() -> void
	{
#if DURIN_WITH_TRACY
		bMappedWriteBacking = true;
		GDynamicBufferWriteProfile.MappedReservedBytes.fetch_add(GetSize(), std::memory_order_relaxed);
		GDynamicBufferWriteProfile.LiveMappedPages.fetch_add(1, std::memory_order_relaxed);
		PublishDynamicBufferWriteProfile();
#endif
	}

	namespace
	{
		using FMappedBufferOwner = TRefCountPtr<FVulkanBuffer>;
		class FVulkanMappedBufferWrite final : public FRHIBufferMappedWrite
		{
		public:
			FVulkanMappedBufferWrite(std::shared_ptr<FRHIDynamicBufferWriteRange> InRange, uint32 InOffset)
				: Range(std::move(InRange)), Offset(InOffset) {}
			auto GetData() const -> void* override
			{ return static_cast<std::byte*>(Range->GetData()) + Offset; }
			auto Commit(IRHICommandContext& Context, FRHIBuffer* Buffer,
				uint32 WriteOffset, uint32 Size) -> void override
			{
				FVulkanBuffer::Cast(Buffer)->CommitMappedWrite(
					static_cast<FVulkanCommandListContext&>(Context), Range, WriteOffset, Size);
			}
		private:
			std::shared_ptr<FRHIDynamicBufferWriteRange> Range;
			uint32 Offset;
		};
	}

	auto FVulkanBuffer::GetBackingBuffer() -> FVulkanBuffer*
	{
		return ActiveWriteRange
			? static_cast<FMappedBufferOwner*>(ActiveWriteRange->GetOwner().get())->GetReference() : this;
	}
	auto FVulkanBuffer::GetBackingBuffer() const -> const FVulkanBuffer*
	{
		return ActiveWriteRange
			? static_cast<const FMappedBufferOwner*>(ActiveWriteRange->GetOwner().get())->GetReference() : this;
	}

	auto FVulkanBuffer::InitializeMappedWrites() -> void
	{
		const auto Required = EBufferUsageFlags::Dynamic | EBufferUsageFlags::CPUWriteDiscard;
		const auto Allowed = Required | EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer;
		if (!EnumHasAllFlags(GetUsage(), Required) || EnumHasAnyFlags(GetUsage(), ~Allowed)
			|| !EnumHasAnyFlags(GetUsage(), EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer)
			|| !GetMappedPointer()) return;
		bSupportsMappedWrites = true;
#if DURIN_WITH_TRACY
		bDiscardWriteFallbackBacking = true;
		GDynamicBufferWriteProfile.FallbackReservedBytes.fetch_add(GetSize(), std::memory_order_relaxed);
#endif
		Device.GetDynamicBufferPool().EnsureInitialPage();
#if DURIN_WITH_TRACY
		PublishDynamicBufferWriteProfile();
#endif
	}

	auto FVulkanBuffer::TryAcquireMappedWrite(uint32 Offset, uint32 Size)
		-> std::shared_ptr<FRHIBufferMappedWrite>
	{
		require(Size && Offset <= GetSize() && Size <= GetSize() - Offset);
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Buffer.TryMappedWrite");
		FRHIDynamicBufferWritePoolSnapshot Snapshot;
		FRHIDynamicBufferWritePoolSnapshot* DiagnosticSnapshot = nullptr;
#if DURIN_WITH_TRACY
		if (TracyIsConnected) DiagnosticSnapshot = &Snapshot;
#endif
		auto Range = bSupportsMappedWrites
			? Device.GetDynamicBufferPool().TryAcquire(Offset + Size, DiagnosticSnapshot)
			: std::shared_ptr<FRHIDynamicBufferWriteRange>{};
#if DURIN_WITH_TRACY
		if (DiagnosticSnapshot)
		{
			auto& Count = Range ? GDynamicBufferWriteProfile.MappedLocks
				: Snapshot.PageCount ? GDynamicBufferWriteProfile.PoolBusyFallbacks : GDynamicBufferWriteProfile.NoPoolFallbacks;
			Count.fetch_add(1, std::memory_order_relaxed);
			GDynamicBufferWriteProfile.RequestedBytes.fetch_add(Size, std::memory_order_relaxed);
			TracyPlot("Vulkan.DynamicBuffer.SampledPoolLiveRanges", static_cast<int64>(Snapshot.RangeCount));
			TracyPlot("Vulkan.DynamicBuffer.SampledPoolFreeBytes", static_cast<int64>(Snapshot.FreeBytes));
			DURIN_PROFILE_CPU_ZONE_TEXT(std::format(
				"buffer={} bytes={} mapped={} liveRanges={} retained={} awaitingRetirement={} pages={} freeBytes={}",
				DebugName, Size, static_cast<bool>(Range), Snapshot.RangeCount, Snapshot.RetainedRangeCount,
				Snapshot.AwaitingRetirementRangeCount, Snapshot.PageCount, Snapshot.FreeBytes));
			PublishDynamicBufferWriteProfile();
		}
#endif
		if (Range)
		{
			auto Desc = FRHIBufferCreateDesc::Create(DebugName.c_str(), GetDesc());
			Desc.Usage &= ~EBufferUsageFlags::CPUWriteDiscard;
			auto Page = static_cast<FMappedBufferOwner*>(Range->GetOwner().get())->GetReference();
			auto Owner = std::make_shared<FMappedBufferOwner>(new FVulkanBuffer(Device, Desc, Page, Range->GetOffset(), Range->GetSize()));
			Range->SetOwner(std::move(Owner));
			return std::make_shared<FVulkanMappedBufferWrite>(std::move(Range), Offset);
		}
		return {};
	}

	auto FVulkanBuffer::PrepareDiscardWrite(IRHICommandContext&) -> void
	{
		CheckVulkanRHIThread();
		// Temporary bytes are consumed on replay, using the original backing and its
		// existing frame synchronization. Never overwrite an in-flight discard version.
		ActiveWriteRange.reset();
		if (bSupportsMappedWrites) Device.GetDynamicBufferPool().ProcessGrowthRequest();
	}

	auto FVulkanBuffer::CommitMappedWrite(FVulkanCommandListContext& Context,
		std::shared_ptr<FRHIDynamicBufferWriteRange> Range, uint32 Offset, uint32 Size) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Buffer.CommitMappedWrite");
		CheckVulkanRHIThread();
		ActiveWriteRange = std::move(Range);
		auto* Backing = GetBackingBuffer();
		Backing->FlushMappedMemory(Offset, Size);
#if DURIN_WITH_TRACY
		if (TracyIsConnected)
		{
			GDynamicBufferWriteProfile.CommittedMappedBytes.fetch_add(Size, std::memory_order_relaxed);
			PublishDynamicBufferWriteProfile();
		}
#endif
		Backing->StateTracker.Apply(Offset, Size, ERHIAccess::HostWrite);
		Context.RetainWriteRange(ActiveWriteRange);
		const std::array Transition{FRHIBufferTransition{
			this, Offset, Size, ERHIAccess::HostWrite, ERHIAccess::TransferWrite}};
		Context.RHITransitionBuffers(Transition);
	}

	auto FVulkanBuffer::InitializeUniformRange(FByteView Data, uint32 Offset) -> void
	{
		CheckVulkanRHIThread();
		require(Offset <= GetSize() && Data.size() <= GetSize() - Offset && GetMappedPointer());
		std::memcpy(static_cast<std::byte*>(GetMappedPointer()) + Offset, Data.data(), Data.size());
		FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
		// A new immutable allocation is first read after submission; flushed host writes
		// are made available by queue submission, including when recorded in a render pass.
		bUniformBacking = true;
	}

	FVulkanBuffer::~FVulkanBuffer()
	{
		CheckVulkanRHIThread();
#if DURIN_WITH_TRACY
		if (bMappedWriteBacking)
		{
			GDynamicBufferWriteProfile.MappedReservedBytes.fetch_sub(GetSize(), std::memory_order_relaxed);
			GDynamicBufferWriteProfile.LiveMappedPages.fetch_sub(1, std::memory_order_relaxed);
		}
		if (bDiscardWriteFallbackBacking)
			GDynamicBufferWriteProfile.FallbackReservedBytes.fetch_sub(GetSize(), std::memory_order_relaxed);
		if (bMappedWriteBacking || bDiscardWriteFallbackBacking) PublishDynamicBufferWriteProfile();
#endif
		if (Buffer && AliasOwner)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer);
		}
		else if (Buffer && Allocation.IsValid())
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
		}
	}

	auto FVulkanBuffer::Write(
		FVulkanCommandListContext& Context,
		uint32 Offset,
		FByteView Data) -> void
	{
		CheckVulkanRHIThread();
		check(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		require(!bUniformBacking);
		if (ActiveWriteRange)
		{
			auto* Backing = GetBackingBuffer();
			if (Offset <= Backing->GetMappedSize() && Data.size() <= Backing->GetMappedSize() - Offset)
			{
				Context.RetainWriteRange(ActiveWriteRange);
				Backing->Write(Context, Offset, Data);
				return;
			}
			// A normal patch can extend a prior discard's defined prefix. Materialize
			// that prefix in the original backing, retaining its exact access states.
			DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Buffer.MaterializeMappedPrefix");
			const auto PreviousRange = ActiveWriteRange;
			const auto PreviousStates = Backing->GetStateTracker().GetIntervals();
			const uint32 PreviousSize = Backing->GetMappedSize();
			require(Allocation.GetMappedData());
			std::memcpy(Allocation.GetMappedData(), Backing->GetMappedPointer(), PreviousSize);
			Device.GetMemoryManager().Flush(Allocation, 0, PreviousSize);
			ActiveWriteRange.reset();
			// Undefined tail bytes keep the logical access contract as well. The
			// original backing owns the full range, so these barriers remain valid.
			StateTracker.Apply(0, GetSize(), ERHIAccess::HostWrite);
			std::vector<FRHIBufferTransition> Restore;
			for (const auto& Interval : PreviousStates)
				if (Interval.Access != ERHIAccess::None && Interval.Access != ERHIAccess::Discard)
					Restore.push_back({this, Interval.Offset, Interval.Size, ERHIAccess::HostWrite, Interval.Access});
				else StateTracker.Apply(Interval.Offset, Interval.Size, Interval.Access);
			if (!Restore.empty()) Context.RHITransitionBuffers(Restore);
			Context.RefreshBufferBindings(this);
		}
		if (void* MappedData = GetMappedPointer(); MappedData != nullptr)
		{
			require(!PageBuffer || (Offset <= PageSize && Data.size() <= PageSize - Offset));
			std::memcpy(static_cast<std::byte*>(MappedData) + Offset, Data.data(), Data.size());
			FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
			StateTracker.Apply(Offset, Data.size(), ERHIAccess::HostWrite);
			const std::array Transition{FRHIBufferTransition{
				this, Offset, Data.size(), ERHIAccess::HostWrite, ERHIAccess::TransferWrite}};
			Context.RHITransitionBuffers(Transition);
			return;
		}

		const uint64 Alignment = std::max<uint64>(16,
			Device.GetGpuProperties().limits.nonCoherentAtomSize);
		FVulkanTransferRange Staging = Context.AcquireTransferRange(
			EVulkanAllocationClassCandidate::TransferUpload, Data.size(), Alignment);
		FVulkanBuffer* StagingBuffer = Staging.GetBuffer();
		GVulkanMemoryBaselineTracker.RecordUpload(Data.size());
		std::memcpy(Staging.GetMappedPointer(), Data.data(), Data.size());
		Staging.Flush();
		StagingBuffer->GetStateTracker().Apply(
			Staging.GetOffset(), Data.size(), ERHIAccess::HostWrite);
		const std::array StagingTransition{FRHIBufferTransition{
			StagingBuffer, Staging.GetOffset(), Data.size(),
			ERHIAccess::HostWrite, ERHIAccess::TransferRead}};
		Context.RHITransitionBuffers(StagingTransition);
		const std::array PreCopyTransition{FRHIBufferTransition{
			this, Offset, Data.size(), ERHIAccess::Discard, ERHIAccess::TransferWrite}};
		Context.RHITransitionBuffers(PreCopyTransition);
		const std::array CopyRegions{FRHIBufferCopyRegion{
			Staging.GetOffset(), Offset, Data.size()}};
		Context.RHICopyBuffer(StagingBuffer, this, CopyRegions);
		Staging.Retire();
	}

	auto FVulkanBuffer::GetMappedPointer() const -> void*
	{
		const auto* Backing = GetBackingBuffer();
		return Backing->PageBuffer
			? static_cast<std::byte*>(Backing->PageBuffer->GetMappedPointer()) + Backing->PageOffset
			: Backing->Allocation.GetMappedData();
	}

	auto FVulkanBuffer::GetMemoryPropertyFlags() const
		-> vk::MemoryPropertyFlags
	{
		const auto* Backing = GetBackingBuffer();
		return Backing->PageBuffer ? Backing->PageBuffer->GetMemoryPropertyFlags()
			: Device.GetMemoryManager().GetMemoryType(Backing->Allocation).propertyFlags;
	}

	auto FVulkanBuffer::FlushMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		auto* Backing = GetBackingBuffer();
		if (Backing->PageBuffer)
		{
			require(Offset <= Backing->PageSize && (!Size || Size <= Backing->PageSize - Offset));
			Backing->PageBuffer->FlushMappedMemory(Backing->PageOffset + Offset, Size ? Size : Backing->PageSize - Offset);
			return;
		}
		const vk::DeviceSize FlushSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Flush(GetBackingBuffer()->Allocation, Offset, FlushSize);
	}

	auto FVulkanBuffer::InvalidateMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		auto* Backing = GetBackingBuffer();
		if (Backing->PageBuffer)
		{
			require(Offset <= Backing->PageSize && (!Size || Size <= Backing->PageSize - Offset));
			Backing->PageBuffer->InvalidateMappedMemory(Backing->PageOffset + Offset, Size ? Size : Backing->PageSize - Offset);
			return;
		}
		const vk::DeviceSize InvalidateSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Invalidate(GetBackingBuffer()->Allocation, Offset, InvalidateSize);
	}

	auto FVulkanDynamicRHI::RHICreateAliasedBuffer(const FRHIBufferCreateDesc& Desc, FRHIBuffer* Source) -> FBufferRHIRef
	{
		if (!Source || !FVulkanBuffer::Cast(Source)->IsAliasable()
			|| EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Dynamic | EBufferUsageFlags::KeepCPUAccessible)
			|| Desc.InitialData.Data) return {};
		FRHIBufferCreateDesc Normalized = Desc;
		if (EnumHasAnyFlags(Normalized.Usage, EBufferUsageFlags::Static)) Normalized.Usage |= EBufferUsageFlags::DestinationCopy;
		if (Source->GetSize() != Normalized.Size || Source->GetStride() != Normalized.Stride
			|| Source->GetUsage() != Normalized.Usage) return {};
		vk::Result Result;
		FBufferRHIRef Buffer = new FVulkanBuffer(*Device, Normalized, false, &Result, FVulkanBuffer::Cast(Source));
		if (Result != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(Result)) ThrowVulkanError(Result, "Vulkan aliased buffer allocation");
			LogVulkanCreationFailure(Result, "aliased buffer", Desc.DebugName ? Desc.DebugName : "");
		}
		return Result == vk::Result::eSuccess ? Buffer : FBufferRHIRef{};
	}

	auto FVulkanDynamicRHI::RHICreateBuffer(FRHICommandListImmediate& RHICmdList,
		const FRHIBufferCreateDesc& CreateDesc)
		-> FBufferRHIRef
	{
		FRHIBufferCreateDesc NormalizedDesc = CreateDesc;
		if (EnumHasAnyFlags(NormalizedDesc.Usage, EBufferUsageFlags::Static)
			|| NormalizedDesc.InitialData.Data != nullptr)
		{
			NormalizedDesc.Usage |= EBufferUsageFlags::DestinationCopy;
		}
		vk::Result CreationResult;
		FBufferRHIRef Result = new FVulkanBuffer(*Device, NormalizedDesc, false, &CreationResult);
		if (CreationResult != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(CreationResult))
				ThrowVulkanError(CreationResult, "Vulkan buffer allocation");
			LogVulkanCreationFailure(CreationResult, "buffer", CreateDesc.DebugName ? CreateDesc.DebugName : "");
			return {};
		}
		auto* CreatedBuffer = FVulkanBuffer::Cast(Result.GetReference());
		CreatedBuffer->InitializeMappedWrites();
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			RHICmdList.WriteBuffer(CreatedBuffer, InitialData.Data, InitialData.Size, 0);
			const ERHIAccess InitialAccess = GetCanonicalBufferAccess(CreateDesc.Usage);
			if (InitialAccess != ERHIAccess::None)
			{
				const std::array Transition{FRHIBufferTransition{
					CreatedBuffer, 0, InitialData.Size, ERHIAccess::TransferWrite, InitialAccess}};
				RHICmdList.TransitionBuffers(Transition);
			}
		}
		return Result;
	}

} // namespace Durin::VulkanRHI
