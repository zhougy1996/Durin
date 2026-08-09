#pragma once

#include "RHIResources.h"
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
		FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc);

		~FVulkanBuffer() override;

		auto IsDynamic() const -> bool;

		auto IsStatic() const -> bool;

		auto GetHandle() const -> vk::Buffer { return Buffer; }

		auto GetMappedPointer() const -> void*;

		auto FlushMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

		auto Write(FVulkanCommandListContext& Context, uint32 Offset, std::span<const uint8> Data) -> void;

		auto GetStateTracker() -> FVulkanBufferStateTracker& { return StateTracker; }
		auto GetStateTracker() const -> const FVulkanBufferStateTracker& { return StateTracker; }

	protected:
		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};

		FVulkanBufferStateTracker StateTracker;

	};

	// Suballocates per-frame uniform ranges from persistently mapped Vulkan buffers.
	class FVulkanDynamicUniformBufferAllocator
	{
	public:
		explicit FVulkanDynamicUniformBufferAllocator(FVulkanDevice& InDevice);

		~FVulkanDynamicUniformBufferAllocator();

		// The rendering thread resets the producer-owned cursor only after the
		// previous RHI serial for this frame slot has completed.
		auto BeginFrameProducer(uint32 FrameIndex) -> void;

		auto TryAllocate(
			uint32 FrameIndex,
			const void* Data,
			uint32 Size,
			FRHIUniformBufferRange& OutRange) -> bool;

		// Device allocation remains RHI-owned. The producer calls this only via
		// an ordered synchronous operation when its prepared pages overflow.
		auto ReservePage(uint32 FrameIndex, uint32 MinSize) -> void;

	private:
		// Owns one persistently mapped backing buffer used for uniform suballocation.
		struct FChunk
		{
			TRefCountPtr<FVulkanBuffer> Buffer;
			uint32 Offset = 0;
		};

		// Tracks the active chunk and write offset independently for each frame in flight.
		struct FFrameState
		{
			std::vector<FChunk> Chunks;
			uint32 CurrentChunkIndex = 0;
		};

		auto CreateChunk(uint32 MinSize) -> FChunk;

		auto GetAlignment() const -> uint32;

		static auto AlignUp(uint32 Value, uint32 Alignment) -> uint32;

		FVulkanDevice& Device;

		std::array<FFrameState, kFrameInFlight> Frames;

	};

	// Owns a host-visible temporary buffer used to transfer data to device-local resources.
	class FStagingBuffer
	{
	public:
		FStagingBuffer(FVulkanDevice& InDevice, uint32 InBufferSize);

		~FStagingBuffer();

		auto GetHandle() const -> vk::Buffer { return Buffer; }

		auto GetSize() const -> uint32 { return BufferSize; }

		auto GetMappedPointer() const -> void*;

		auto FlushMappedMemory() -> void;

	private:
		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		// The size of the staging buffer in bytes.
		uint32 BufferSize{};

		FVulkanAllocation Allocation{};
	};
} // namespace Durin::VulkanRHI
