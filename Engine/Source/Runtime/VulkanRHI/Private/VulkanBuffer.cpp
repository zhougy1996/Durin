#include "VulkanCreation.h"
#include "VulkanCreationTiming.h"
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
			throw vk::SystemError(vk::make_error_code(Result), std::format(
				"Vulkan buffer allocation failed: result={}, size={}, usage={}, allocationClass={}",
				vk::to_string(Result), BufferInfo.size,
				vk::to_string(BufferInfo.usage), static_cast<uint32>(AllocationCandidate)));
		}
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::ResourcePublication);
#endif
			Device.GetRHI().GetDebugUtils().NameObject(Buffer, DebugName);
		}
		catch (...)
		{
			MemoryManager.DestroyBuffer(Allocation, Buffer);
			Buffer = nullptr;
			throw;
		}
	}

	auto FVulkanBuffer::InitializeDeferredReadOnly(FByteView Data, uint32 Offset) -> void
	{
		CheckVulkanRHIThread();
		require(Offset <= GetSize() && Data.size() <= GetSize() - Offset && GetMappedPointer());
		std::memcpy(static_cast<std::byte*>(GetMappedPointer()) + Offset, Data.data(), Data.size());
		FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
		// A new immutable allocation is first read after submission; flushed host writes
		// are made available by queue submission, including when recorded in a render pass.
		bDeferredReadOnly = true;
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
		WriteImpl(Context, Offset, Data, true);
	}

	auto FVulkanBuffer::Upload(
		FVulkanCommandListContext& Context,
		uint32 Offset,
		FByteView Data) -> void
	{
		WriteImpl(Context, Offset, Data, false);
	}

	auto FVulkanBuffer::WriteImpl(
		FVulkanCommandListContext& Context,
		uint32 Offset,
		FByteView Data,
		bool bRestoreCanonicalAccess) -> void
	{
		CheckVulkanRHIThread();
		check(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		require(!bDeferredReadOnly);
		if (void* MappedData = Allocation.GetMappedData(); MappedData != nullptr)
		{
			std::memcpy(static_cast<std::byte*>(MappedData) + Offset, Data.data(), Data.size());
			FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
			StateTracker.Apply(Offset, Data.size(), ERHIAccess::HostWrite);
			const ERHIAccess FinalAccess = bRestoreCanonicalAccess
				? GetCanonicalBufferAccess(GetUsage()) : ERHIAccess::TransferWrite;
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

		const ERHIAccess FinalAccess = bRestoreCanonicalAccess
			? GetCanonicalBufferAccess(GetUsage()) : ERHIAccess::TransferWrite;
		if (bRestoreCanonicalAccess && FinalAccess != ERHIAccess::None)
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

	auto FVulkanDynamicRHI::RHITryCreateBuffer(FRHICommandListImmediate& RHICmdList,
		const FRHIBufferCreateDesc& CreateDesc)
		-> std::expected<FBufferRHIRef, FRHICreationError>
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		FVulkanCreationTimingScope TimingScope(EVulkanCreationKind::Buffer);
#endif
		FRHIBufferCreateDesc NormalizedDesc = CreateDesc;
		if (EnumHasAnyFlags(NormalizedDesc.Usage, EBufferUsageFlags::Static)
			|| NormalizedDesc.InitialData.Data != nullptr)
		{
			NormalizedDesc.Usage |= EBufferUsageFlags::DestinationCopy;
		}
		auto Result = TryCreateVulkanResource([&]() -> FBufferRHIRef {
			return new FVulkanBuffer(*Device, NormalizedDesc);
		});
		if (!Result) return std::unexpected(Result.error());
		auto* CreatedBuffer = FVulkanBuffer::Cast(Result->GetReference());
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			if (!RHICmdList.TryWriteBuffer(CreatedBuffer, 0,
				{static_cast<const std::byte*>(InitialData.Data), InitialData.Size}))
				return std::unexpected(FRHICreationError{
					.Failure = ERHIResourceCreationFailure::ResourceExhausted,
					.Source = ERHICreationFailureSource::RequestNotAdmitted});
		}
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		if (auto* Timing = TimingScope.Get()) Timing->bSucceeded = !!Result;
#endif
		return Result;
	}

} // namespace Durin::VulkanRHI
