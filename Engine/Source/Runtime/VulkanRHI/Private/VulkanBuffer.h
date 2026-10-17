#pragma once

#include "CoreMinimal.h"
#include "VulkanRHIAPI.h"

#include "RHI.h"

#include "VulkanPlatform.h"

#include "RHIResources.h"
#include "RHICompletion.h"
#include "VulkanMemory.h"
#include "VulkanResourceState.h"
#include "Backend/RHIDynamicBufferWrite.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandListContext;

	// Owns a Vulkan buffer allocation and enforces its CPU lock lifecycle.
	class FVulkanBuffer : public FRHIBuffer
	{
	public:
		static auto Cast(FRHIBuffer* Resource) -> FVulkanBuffer*
		{
			require(Resource);
			return static_cast<FVulkanBuffer*>(Resource);
		}
		static auto Cast(const FRHIBuffer* Resource) -> const FVulkanBuffer*
		{
			require(Resource);
			return static_cast<const FVulkanBuffer*>(Resource);
		}
		// Nullable factories check OutResult before publication. Required internal
		// allocations omit it and propagate native failures.
		VULKANRHI_API FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc,
			bool bConcurrentReadOnly = false, vk::Result* OutResult = nullptr, FVulkanBuffer* AliasSource = nullptr);

		VULKANRHI_API ~FVulkanBuffer() override;
		// CPU-only logical slice; no native allocation or mapping calls.
		VULKANRHI_API FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& Desc,
			TRefCountPtr<FVulkanBuffer> Page, uint32 Offset, uint32 MappedSize);
		auto MarkMappedPage() -> void;

		auto GetHandle() const -> vk::Buffer
		{ const auto* Backing = GetBackingBuffer(); return Backing->PageBuffer ? Backing->PageBuffer->GetHandle() : Backing->Buffer; }
		auto GetBackingOffset() const -> uint32 { return GetBackingBuffer()->PageOffset; }
		auto GetMappedSize() const -> uint32
		{ const auto* Backing = GetBackingBuffer(); return Backing->PageBuffer ? Backing->PageSize : Backing->GetSize(); }
		auto GetNativeRangeSize(uint64 Offset, uint64 Size) const -> uint64
		{ const uint64 Available = GetMappedSize(); return Offset < Available ? std::min(Size, Available - Offset) : 0; }
		VULKANRHI_API auto GetBackingBuffer() -> FVulkanBuffer*;
		VULKANRHI_API auto GetBackingBuffer() const -> const FVulkanBuffer*;
		auto GetWriteRange() const -> const std::shared_ptr<FRHIDynamicBufferWriteRange>& { return ActiveWriteRange; }
		auto InitializeMappedWrites() -> void;
		auto TryAcquireMappedWrite(uint32 Offset, uint32 Size)
			-> std::shared_ptr<FRHIBufferMappedWrite> override;
		auto PrepareDiscardWrite(IRHICommandContext& Context) -> void override;
		auto CommitMappedWrite(FVulkanCommandListContext& Context,
			std::shared_ptr<FRHIDynamicBufferWriteRange> Range, uint32 Offset, uint32 Size) -> void;
		auto GetDebugName() const -> std::string_view { return DebugName; }
		auto IsAliasable() const -> bool { return bAllowAliasing; }
		auto SharesBackingAllocationWith(const FVulkanBuffer& Other) const -> bool
		{ return Allocation.IsValid() && Allocation.Handle == Other.Allocation.Handle; }

		VULKANRHI_API auto GetMappedPointer() const -> void*;
		auto GetAllocationClass() const -> EVulkanAllocationClassCandidate
		{
			const auto* Backing = GetBackingBuffer();
			return Backing->PageBuffer ? Backing->PageBuffer->GetAllocationClass() : Backing->Allocation.Class;
		}
		VULKANRHI_API auto GetMemoryPropertyFlags() const -> vk::MemoryPropertyFlags;

		VULKANRHI_API auto FlushMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;
		VULKANRHI_API auto InvalidateMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

		auto Write(FVulkanCommandListContext& Context, uint32 Offset, FByteView Data) -> void;
		auto InitializeUniformRange(FByteView Data, uint32 Offset = 0) -> void;

		auto GetStateTracker() -> FVulkanBufferStateTracker& { return GetBackingBuffer()->StateTracker; }
		auto GetStateTracker() const -> const FVulkanBufferStateTracker& { return GetBackingBuffer()->StateTracker; }

	protected:
		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};
		TRefCountPtr<FVulkanBuffer> AliasOwner;
		bool bAllowAliasing = false;

		FVulkanBufferStateTracker StateTracker;
		std::string DebugName;
		bool bUniformBacking = false;
		bool bMappedWriteBacking = false;
		bool bDiscardWriteFallbackBacking = false;
		bool bSupportsMappedWrites = false;
		TRefCountPtr<FVulkanBuffer> PageBuffer;
		uint32 PageOffset = 0;
		uint32 PageSize = 0;
		std::shared_ptr<FRHIDynamicBufferWriteRange> ActiveWriteRange;

	};

} // namespace Durin::VulkanRHI
