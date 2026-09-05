#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanContext.h"

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
			return Access;
		}
	}

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
		, StateTracker(InCreateDesc.Size)
		, DebugName(InCreateDesc.DebugName ? InCreateDesc.DebugName :
			Device.GetRHI().GetDebugUtils().MakeInternalName("Buffer"))
	{
		CheckVulkanRHIThread();
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InCreateDesc.Size);
		BufferInfo.setUsage(ToVulkan_BufferUsageFlags(InCreateDesc.Usage));
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

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
		if (Result != vk::Result::eSuccess)
		{
			throw std::runtime_error(std::format(
				"Vulkan buffer allocation failed: result={}, size={}, usage={}, allocationClass={}",
				vk::to_string(Result), BufferInfo.size,
				vk::to_string(BufferInfo.usage), static_cast<uint32>(AllocationCandidate)));
		}
		Device.GetRHI().GetDebugUtils().NameObject(Buffer, DebugName);
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
		if (void* MappedData = Allocation.GetMappedData(); MappedData != nullptr)
		{
			std::memcpy(static_cast<std::byte*>(MappedData) + Offset, Data.data(), Data.size());
			FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
			StateTracker.Apply(Offset, Data.size(), ERHIAccess::HostWrite);
			const ERHIAccess FinalAccess = GetCanonicalBufferAccess(GetUsage());
			if (FinalAccess != ERHIAccess::None)
			{
				const std::array Transition{FRHIBufferTransition{
					this, Offset, Data.size(), ERHIAccess::HostWrite, FinalAccess}};
				Context.RHITransitionBuffers(Transition);
			}
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

		const ERHIAccess FinalAccess = GetCanonicalBufferAccess(GetUsage());
		if (FinalAccess != ERHIAccess::None)
		{
			const std::array PostCopyTransition{FRHIBufferTransition{
				this, Offset, Data.size(), ERHIAccess::TransferWrite, FinalAccess}};
			Context.RHITransitionBuffers(PostCopyTransition);
		}
		Staging.Retire();
	}

	auto FVulkanBuffer::IsDynamic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Dynamic);
	}

	auto FVulkanBuffer::IsStatic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Static);
	}

	auto FVulkanBuffer::GetMappedPointer() const -> void*
	{
		return Allocation.GetMappedData();
	}

	auto FVulkanBuffer::GetMemoryPropertyFlags() const
		-> vk::MemoryPropertyFlags
	{
		return Device.GetMemoryManager().GetMemoryType(Allocation).propertyFlags;
	}

	auto FVulkanBuffer::FlushMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		const vk::DeviceSize FlushSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Flush(Allocation, Offset, FlushSize);
	}

	auto FVulkanBuffer::InvalidateMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		const vk::DeviceSize InvalidateSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Invalidate(Allocation, Offset, InvalidateSize);
	}

	FVulkanDynamicUniformBufferAllocator::FVulkanDynamicUniformBufferAllocator(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		CheckVulkanRHIThread();
		for (FProducerState& State : ProducerStates)
		{
			State.Chunks.push_back(CreateChunk(4 * 1024 * 1024));
		}
	}

	FVulkanDynamicUniformBufferAllocator::~FVulkanDynamicUniformBufferAllocator()
	{
		for (FProducerState& State : ProducerStates)
		{
			for (FChunk& Chunk : State.Chunks)
			{
				GVulkanMemoryBaselineTracker.RecordArenaPageFreed(
					EVulkanAllocationClassCandidate::DynamicUpload,
					Chunk.Buffer->GetSize());
			}
		}
	}

	auto FVulkanDynamicUniformBufferAllocator::FProducerState::GetLastUseToken()
		const -> FVulkanCompletionToken
	{
		FVulkanCompletionToken Result = 0;
		for (const FChunk& Chunk : Chunks)
		{
			Result = std::max(Result, Chunk.LastUseToken);
		}
		return Result;
	}

	auto FVulkanDynamicUniformBufferAllocator::PrepareForProducer() -> void
	{
		CheckVulkanRHIThread();
		check(ActiveProducerIndex == std::numeric_limits<uint32>::max());
		auto& Tracker = Device.GetCompletionTracker();
		Tracker.Poll();
		FVulkanCompletionToken Completed = Tracker.GetCompletedToken();
		for (uint32 Offset = 0; Offset < ProducerStates.size(); ++Offset)
		{
			const uint32 Index = (NextProducerIndex + Offset)
				% static_cast<uint32>(ProducerStates.size());
			if (ProducerStates[Index].GetLastUseToken() <= Completed)
			{
				ActiveProducerIndex = Index;
				break;
			}
		}
		if (ActiveProducerIndex == std::numeric_limits<uint32>::max())
		{
			auto Oldest = std::ranges::min_element(ProducerStates, {},
				&FProducerState::GetLastUseToken);
			check(Oldest != ProducerStates.end()
				&& Oldest->GetLastUseToken() > 0);
			GVulkanMemoryBaselineTracker.RecordArenaWait(
				EVulkanAllocationClassCandidate::DynamicUpload);
			Tracker.WaitForToken(Oldest->GetLastUseToken());
			Completed = Tracker.GetCompletedToken();
			ActiveProducerIndex = static_cast<uint32>(
				std::distance(ProducerStates.begin(), Oldest));
		}
		FProducerState& State = ProducerStates[ActiveProducerIndex];
		check(State.GetLastUseToken() <= Completed && !State.Chunks.empty());
		State.CurrentChunkIndex = 0;
		for (FChunk& Chunk : State.Chunks)
		{
			check(!Chunk.bUsed && Chunk.LastUseToken <= Completed);
			if (Chunk.LiveRequestedBytes > 0)
			{
				GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
					EVulkanAllocationClassCandidate::DynamicUpload,
					Chunk.LiveRequestedBytes);
			}
			Chunk.Offset = 0;
			Chunk.LastUseToken = 0;
			Chunk.LiveRequestedBytes = 0;
		}
		NextProducerIndex = (ActiveProducerIndex + 1)
			% static_cast<uint32>(ProducerStates.size());
	}

	auto FVulkanDynamicUniformBufferAllocator::RetireProducer(
		FVulkanCompletionToken Token) -> void
	{
		CheckVulkanRHIThread();
		check(Token > 0);
		if (ActiveProducerIndex >= ProducerStates.size())
		{
			return;
		}
		for (FChunk& Chunk : ProducerStates[ActiveProducerIndex].Chunks)
		{
			if (Chunk.bUsed)
			{
				Chunk.LastUseToken = std::max(Chunk.LastUseToken, Token);
				Chunk.bUsed = false;
			}
		}
		ActiveProducerIndex = std::numeric_limits<uint32>::max();
	}

	auto FVulkanDynamicUniformBufferAllocator::TryAllocate(
		const void* Data,
		uint32 Size,
		FRHIUniformBufferRange& OutRange) -> bool
	{
		check(Data);
		check(Size > 0);

		const uint32 Alignment = GetAlignment();
		const uint32 AllocationSize = AlignUp(Size, Alignment);
		check(ActiveProducerIndex < ProducerStates.size());
		FProducerState& State = ProducerStates[ActiveProducerIndex];
		check(!State.Chunks.empty());

		FChunk* Chunk = &State.Chunks[State.CurrentChunkIndex];
		if (Chunk->Offset + AllocationSize > Chunk->Buffer->GetSize())
		{
			bool bFoundReusableChunk = false;
			for (uint32 ChunkIndex = State.CurrentChunkIndex + 1;
				ChunkIndex < State.Chunks.size(); ++ChunkIndex)
			{
				if (State.Chunks[ChunkIndex].Buffer->GetSize() >= AllocationSize)
				{
					State.CurrentChunkIndex = ChunkIndex;
					Chunk = &State.Chunks[ChunkIndex];
					bFoundReusableChunk = true;
					break;
				}
			}

			if (!bFoundReusableChunk)
			{
				return false;
			}
		}

		const uint32 AllocationOffset = AlignUp(Chunk->Offset, Alignment);
		check(AllocationOffset + Size <= Chunk->Buffer->GetSize());
		void* MappedPointer = Chunk->Buffer->GetMappedPointer();
		check(MappedPointer);
		std::memcpy(static_cast<std::byte*>(MappedPointer) + AllocationOffset, Data, Size);
		Chunk->Buffer->FlushMappedMemory(AllocationOffset, Size);
		Chunk->Offset = AllocationOffset + AllocationSize;
		Chunk->bUsed = true;
		Chunk->LiveRequestedBytes = FVulkanMemoryBaselineTracker::SaturatingAdd(
			Chunk->LiveRequestedBytes, Size);
		GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(
			EVulkanAllocationClassCandidate::DynamicUpload, Size,
			Chunk->bHasServedAllocation,
			Chunk->Buffer->GetSize() > 4 * 1024 * 1024);
		Chunk->bHasServedAllocation = true;

		OutRange = FRHIUniformBufferRange{
			.Buffer = Chunk->Buffer.GetReference(),
			.Offset = AllocationOffset,
			.Size = Size
		};
		return true;
	}

	auto FVulkanDynamicUniformBufferAllocator::ReservePage(uint32 MinSize) -> void
	{
		CheckVulkanRHIThread();
		check(ActiveProducerIndex < ProducerStates.size());
		constexpr uint32 MaxChunksPerProducer = 8;
		auto& Chunks = ProducerStates[ActiveProducerIndex].Chunks;
		checkf(Chunks.size() < MaxChunksPerProducer,
			"Vulkan dynamic-uniform page bound reached: producer={}, pages={}, requestedBytes={}.",
			ActiveProducerIndex, Chunks.size(), MinSize);
		GVulkanMemoryBaselineTracker.RecordArenaOverflow(
			EVulkanAllocationClassCandidate::DynamicUpload);
		Chunks.push_back(CreateChunk(MinSize));
	}

	auto FVulkanDynamicUniformBufferAllocator::GetProducerTokensForTesting()
		const -> std::array<FVulkanCompletionToken, kFrameInFlight>
	{
		std::array<FVulkanCompletionToken, kFrameInFlight> Result{};
		for (uint32 Index = 0; Index < ProducerStates.size(); ++Index)
		{
			Result[Index] = ProducerStates[Index].GetLastUseToken();
		}
		return Result;
	}

	auto FVulkanDynamicUniformBufferAllocator::CreateChunk(uint32 MinSize) -> FChunk
	{
		CheckVulkanRHIThread();
		constexpr uint32 DefaultChunkSize = 4 * 1024 * 1024;
		const uint32 ChunkSize = std::max(DefaultChunkSize, AlignUp(MinSize, GetAlignment()));
		FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::Create(
			"DynamicUniformBufferArena",
			ChunkSize,
			0,
			EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Dynamic | EBufferUsageFlags::Volatile
		);
		FChunk Chunk;
		Chunk.Buffer = new FVulkanBuffer(Device, CreateDesc);
		GVulkanMemoryBaselineTracker.RecordArenaPageAllocated(
			EVulkanAllocationClassCandidate::DynamicUpload, ChunkSize);
		return Chunk;
	}

	auto FVulkanDynamicUniformBufferAllocator::GetAlignment() const -> uint32
	{
		const uint32 VulkanAlignment = static_cast<uint32>(Device.GetGpuProperties().limits.minUniformBufferOffsetAlignment);
		return std::max(16u, VulkanAlignment);
	}

	auto FVulkanDynamicUniformBufferAllocator::AlignUp(uint32 Value, uint32 Alignment) -> uint32
	{
		check(Alignment > 0);
		return (Value + Alignment - 1) / Alignment * Alignment;
	}

	FVulkanDynamicStorageBufferAllocator::FVulkanDynamicStorageBufferAllocator(
		FVulkanDevice& InDevice) : Device(InDevice)
	{
		CheckVulkanRHIThread();
		for (uint32 FrameIndex = 0; FrameIndex < Frames.size(); ++FrameIndex)
			ReservePage(FrameIndex, 4 * 1024 * 1024);
	}

	FVulkanDynamicStorageBufferAllocator::~FVulkanDynamicStorageBufferAllocator() = default;

	auto FVulkanDynamicStorageBufferAllocator::BeginFrameProducer(uint32 FrameIndex) -> void
	{
		FFrameState& Frame = Frames[FrameIndex % kFrameInFlight];
		Frame.CurrentChunkIndex = 0;
		Frame.RequestedBytes = 0;
		for (FChunk& Chunk : Frame.Chunks) Chunk.Offset = 0;
	}

	auto FVulkanDynamicStorageBufferAllocator::TryAllocate(
		uint32 FrameIndex, const void* Data, uint32 Size,
		FRHIStorageBufferRange& OutRange) -> bool
	{
		if (!Data || Size == 0
			|| Size > Device.GetGpuProperties().limits.maxStorageBufferRange
			|| Size > MaximumBytesPerFrame) return false;
		const uint32 Alignment = GetAlignment();
		const uint32 AllocationSize = AlignUp(Size, Alignment);
		FFrameState& Frame = Frames[FrameIndex % kFrameInFlight];
		if (Frame.RequestedBytes + AllocationSize > MaximumBytesPerFrame
			|| Frame.Chunks.empty()) return false;
		FChunk* Chunk = &Frame.Chunks[Frame.CurrentChunkIndex];
		if (AlignUp(Chunk->Offset, Alignment) + Size > Chunk->Buffer->GetSize())
		{
			bool bFound = false;
			for (uint32 Index = Frame.CurrentChunkIndex + 1; Index < Frame.Chunks.size(); ++Index)
				if (Frame.Chunks[Index].Buffer->GetSize() >= AllocationSize)
				{
					Frame.CurrentChunkIndex = Index;
					Chunk = &Frame.Chunks[Index];
					bFound = true;
					break;
				}
			if (!bFound) return false;
		}
		const uint32 Offset = AlignUp(Chunk->Offset, Alignment);
		std::memcpy(static_cast<std::byte*>(Chunk->Buffer->GetMappedPointer()) + Offset, Data, Size);
		Chunk->Buffer->FlushMappedMemory(Offset, Size);
		Chunk->Buffer->GetStateTracker().Apply(
			Offset, Size, ERHIAccess::HostWrite);
		Chunk->Offset = Offset + AllocationSize;
		Frame.RequestedBytes += AllocationSize;
		OutRange = {Chunk->Buffer.GetReference(), Offset, Size};
		return true;
	}

	auto FVulkanDynamicStorageBufferAllocator::ReservePage(
		uint32 FrameIndex, uint32 MinSize) -> void
	{
		CheckVulkanRHIThread();
		FFrameState& Frame = Frames[FrameIndex % kFrameInFlight];
		if (MinSize == 0 || MinSize > MaximumBytesPerFrame
			|| Frame.Chunks.size() >= MaximumChunksPerFrame) return;
		Frame.Chunks.push_back(CreateChunk(MinSize));
	}

	auto FVulkanDynamicStorageBufferAllocator::CreateChunk(uint32 MinSize) -> FChunk
	{
		const uint32 ChunkSize = std::max(4u * 1024u * 1024u,
			AlignUp(MinSize, GetAlignment()));
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			"DynamicStorageBufferArena", ChunkSize, 0,
			EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::ShaderResource
				| EBufferUsageFlags::Dynamic
				| EBufferUsageFlags::Volatile);
		return {TRefCountPtr<FVulkanBuffer>(new FVulkanBuffer(Device, Desc)), 0};
	}

	auto FVulkanDynamicStorageBufferAllocator::GetAlignment() const -> uint32
	{
		return std::max(16u, static_cast<uint32>(
			Device.GetGpuProperties().limits.minStorageBufferOffsetAlignment));
	}

	auto FVulkanDynamicStorageBufferAllocator::AlignUp(uint32 Value, uint32 Alignment) -> uint32
	{
		check(Alignment > 0 && Value <= std::numeric_limits<uint32>::max() - Alignment + 1);
		return (Value + Alignment - 1) / Alignment * Alignment;
	}

	auto FVulkanDynamicRHI::RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer>
	{
		FRHIBufferCreateDesc NormalizedDesc = CreateDesc;
		if (EnumHasAnyFlags(NormalizedDesc.Usage, EBufferUsageFlags::Static)
			|| NormalizedDesc.InitialData.Data != nullptr)
		{
			NormalizedDesc.Usage |= EBufferUsageFlags::DestinationCopy;
		}
		TRefCountPtr<FRHIBuffer> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, NormalizedDesc, &Result]() {
					Result = new FVulkanBuffer(*Device, NormalizedDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI buffer '{}': {}",
				CreateDesc.DebugName ? CreateDesc.DebugName : "<unnamed>",
				CreationResult.Diagnostic);
			return nullptr;
		}
		auto* CreatedBuffer = static_cast<FVulkanBuffer*>(Result.GetReference());
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			RHICmdList.WriteBuffer(CreatedBuffer, InitialData.Data, InitialData.Size, 0);
		}
		return Result;
	}

} // namespace Durin::VulkanRHI
