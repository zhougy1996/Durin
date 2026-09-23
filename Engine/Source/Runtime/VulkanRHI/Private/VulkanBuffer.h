#pragma once

#include "RHIResources.h"
#include "RHICompletion.h"
#include "VulkanMemory.h"
#include "VulkanResourceState.h"

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
			require(Resource && !IsCPUAuthoredBufferResource(Resource));
			return static_cast<FVulkanBuffer*>(Resource);
		}
		static auto Cast(const FRHIBuffer* Resource) -> const FVulkanBuffer*
		{
			require(Resource && !IsCPUAuthoredBufferResource(Resource));
			return static_cast<const FVulkanBuffer*>(Resource);
		}
		FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc);

		~FVulkanBuffer() override;

		auto IsDynamic() const -> bool;

		auto IsStatic() const -> bool;

		auto GetHandle() const -> vk::Buffer { return Buffer; }
		auto GetDebugName() const -> std::string_view { return DebugName; }

		VULKANRHI_API auto GetMappedPointer() const -> void*;
		auto GetAllocationClass() const -> EVulkanAllocationClassCandidate
		{
			return Allocation.Class;
		}
		VULKANRHI_API auto GetMemoryPropertyFlags() const -> vk::MemoryPropertyFlags;

		VULKANRHI_API auto FlushMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;
		VULKANRHI_API auto InvalidateMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

		auto Write(FVulkanCommandListContext& Context, uint32 Offset, FByteView Data) -> void;
		auto Upload(FVulkanCommandListContext& Context, uint32 Offset, FByteView Data) -> void;
		auto InitializeDeferredReadOnly(FByteView Data, uint32 Offset = 0) -> void;
		auto IsDeferredReadOnly() const -> bool { return bDeferredReadOnly; }

		auto GetStateTracker() -> FVulkanBufferStateTracker& { return StateTracker; }
		auto GetStateTracker() const -> const FVulkanBufferStateTracker& { return StateTracker; }

	protected:
		auto WriteImpl(FVulkanCommandListContext& Context, uint32 Offset,
			FByteView Data, bool bRestoreCanonicalAccess) -> void;
		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};

		FVulkanBufferStateTracker StateTracker;
		std::string DebugName;
		bool bDeferredReadOnly = false;

	};

	// Suballocates per-frame uniform ranges from persistently mapped Vulkan buffers.
} // namespace Durin::VulkanRHI
