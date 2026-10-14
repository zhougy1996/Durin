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
#include "VulkanQueue.h"

namespace Durin::VulkanRHI
{
	namespace
	{
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
		bool bConcurrentReadOnly, vk::Result* OutResult)
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
		const vk::Result Result = MemoryManager.CreateBuffer(
			Allocation, Buffer, AllocationCandidate, BufferInfo, DebugName.c_str());
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

	auto FVulkanBuffer::InitializeMappedWrites(const FRHIBufferCreateDesc& CreateDesc) -> void
	{
		const auto Required = EBufferUsageFlags::Dynamic | EBufferUsageFlags::CPUWriteDiscard;
		const auto Allowed = Required | EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer;
		if (!EnumHasAllFlags(GetUsage(), Required) || EnumHasAnyFlags(GetUsage(), ~Allowed)
			|| !EnumHasAnyFlags(GetUsage(), EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer)
			|| !GetMappedPointer()) return;
		// Three bounded discard versions plus the original temporary-upload backing.
		// Allocate only for explicitly opted-in buffers, before they are published.
		FRHIBufferCreateDesc BackingDesc = CreateDesc;
		BackingDesc.Usage &= ~EBufferUsageFlags::CPUWriteDiscard;
		BackingDesc.InitialData = {};
		for (uint32 Index = 0; Index != 3; ++Index)
		{
			vk::Result Result;
			auto Owner = std::make_shared<FMappedBufferOwner>(
				new FVulkanBuffer(Device, BackingDesc, false, &Result));
			if (Result != vk::Result::eSuccess)
			{
				if (!IsRecoverableVulkanCreationError(Result)) ThrowVulkanError(Result, "Mapped buffer allocation");
				break;
			}
			if (void* Mapped = (*Owner)->GetMappedPointer()) WritePool.Add(Owner, Mapped);
		}
	}

	auto FVulkanBuffer::TryAcquireMappedWrite(uint32 Offset, uint32 Size)
		-> std::shared_ptr<FRHIBufferMappedWrite>
	{
		require(Size && Offset <= GetSize() && Size <= GetSize() - Offset);
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Buffer.TryMappedWrite");
		if (auto Range = WritePool.TryAcquire())
			return std::make_shared<FVulkanMappedBufferWrite>(std::move(Range), Offset);
		return {};
	}

	auto FVulkanBuffer::PrepareDiscardWrite(IRHICommandContext&) -> void
	{
		CheckVulkanRHIThread();
		// Temporary bytes are consumed on replay, using the original backing and its
		// existing frame synchronization. Never overwrite an in-flight discard version.
		ActiveWriteRange.reset();
	}

	auto FVulkanBuffer::CommitMappedWrite(FVulkanCommandListContext& Context,
		std::shared_ptr<FRHIDynamicBufferWriteRange> Range, uint32 Offset, uint32 Size) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Buffer.CommitMappedWrite");
		CheckVulkanRHIThread();
		ActiveWriteRange = std::move(Range);
		auto* Backing = GetBackingBuffer();
		Backing->FlushMappedMemory(Offset, Size);
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
		if (Buffer && Allocation.IsValid())
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
			Context.RetainWriteRange(ActiveWriteRange);
			GetBackingBuffer()->Write(Context, Offset, Data);
			return;
		}
		if (void* MappedData = Allocation.GetMappedData(); MappedData != nullptr)
		{
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
		return GetBackingBuffer()->Allocation.GetMappedData();
	}

	auto FVulkanBuffer::GetMemoryPropertyFlags() const
		-> vk::MemoryPropertyFlags
	{
		return Device.GetMemoryManager().GetMemoryType(GetBackingBuffer()->Allocation).propertyFlags;
	}

	auto FVulkanBuffer::FlushMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		const vk::DeviceSize FlushSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Flush(GetBackingBuffer()->Allocation, Offset, FlushSize);
	}

	auto FVulkanBuffer::InvalidateMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		const vk::DeviceSize InvalidateSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Invalidate(GetBackingBuffer()->Allocation, Offset, InvalidateSize);
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
		CreatedBuffer->InitializeMappedWrites(NormalizedDesc);
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
