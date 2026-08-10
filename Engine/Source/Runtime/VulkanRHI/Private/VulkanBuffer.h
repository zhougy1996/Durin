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
		auto InvalidateMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

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

	class FVulkanDynamicStorageBufferAllocator
	{
	public:
		static constexpr uint32 MaximumChunksPerFrame = 16;
		static constexpr uint64 MaximumBytesPerFrame = 64ull * 1024ull * 1024ull;
		explicit FVulkanDynamicStorageBufferAllocator(FVulkanDevice& InDevice);
		~FVulkanDynamicStorageBufferAllocator();
		auto BeginFrameProducer(uint32 FrameIndex) -> void;
		auto TryAllocate(uint32 FrameIndex, const void* Data, uint32 Size,
			FRHIStorageBufferRange& OutRange) -> bool;
		auto ReservePage(uint32 FrameIndex, uint32 MinSize) -> void;

	private:
		struct FChunk
		{
			TRefCountPtr<FVulkanBuffer> Buffer;
			uint32 Offset = 0;
		};
		struct FFrameState
		{
			std::vector<FChunk> Chunks;
			uint32 CurrentChunkIndex = 0;
			uint64 RequestedBytes = 0;
		};
		auto CreateChunk(uint32 MinSize) -> FChunk;
		auto GetAlignment() const -> uint32;
		static auto AlignUp(uint32 Value, uint32 Alignment) -> uint32;
		FVulkanDevice& Device;
		std::array<FFrameState, kFrameInFlight> Frames;
	};

} // namespace Durin::VulkanRHI
