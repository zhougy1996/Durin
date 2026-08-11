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
		auto GetDebugName() const -> std::string_view { return DebugName; }

		VULKANRHI_API auto GetMappedPointer() const -> void*;
		auto GetAllocationClass() const -> EVulkanAllocationClassCandidate
		{
			return Allocation.Class;
		}
		VULKANRHI_API auto GetMemoryPropertyFlags() const -> vk::MemoryPropertyFlags;

		VULKANRHI_API auto FlushMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;
		VULKANRHI_API auto InvalidateMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

		auto Write(FVulkanCommandListContext& Context, uint32 Offset, std::span<const uint8> Data) -> void;

		auto GetStateTracker() -> FVulkanBufferStateTracker& { return StateTracker; }
		auto GetStateTracker() const -> const FVulkanBufferStateTracker& { return StateTracker; }

	protected:
		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};

		FVulkanBufferStateTracker StateTracker;
		std::string DebugName;

	};

	// Suballocates per-frame uniform ranges from persistently mapped Vulkan buffers.
	class FVulkanDynamicUniformBufferAllocator
	{
	public:
		explicit FVulkanDynamicUniformBufferAllocator(FVulkanDevice& InDevice);

		~FVulkanDynamicUniformBufferAllocator();

		// Selects a producer state only after every used page token has completed.
		auto PrepareForProducer() -> void;
		auto RetireProducer(FVulkanCompletionToken Token) -> void;

		auto TryAllocate(
			const void* Data,
			uint32 Size,
			FRHIUniformBufferRange& OutRange) -> bool;

		// Device allocation remains RHI-owned. The producer calls this only via
		// an ordered synchronous operation when its prepared pages overflow.
		auto ReservePage(uint32 MinSize) -> void;
		auto GetProducerTokensForTesting() const
			-> std::array<FVulkanCompletionToken, kFrameInFlight>;

	private:
		// Owns one persistently mapped backing buffer used for uniform suballocation.
		struct FChunk
		{
			TRefCountPtr<FVulkanBuffer> Buffer;
			uint32 Offset = 0;
			FVulkanCompletionToken LastUseToken = 0;
			bool bUsed = false;
			bool bHasServedAllocation = false;
			uint64 LiveRequestedBytes = 0;
		};

		// Tracks the active chunk and write offset independently for each frame in flight.
		struct FProducerState
		{
			std::vector<FChunk> Chunks;
			uint32 CurrentChunkIndex = 0;
			auto GetLastUseToken() const -> FVulkanCompletionToken;
		};

		auto CreateChunk(uint32 MinSize) -> FChunk;

		auto GetAlignment() const -> uint32;

		static auto AlignUp(uint32 Value, uint32 Alignment) -> uint32;

		FVulkanDevice& Device;

		std::array<FProducerState, kFrameInFlight> ProducerStates;
		uint32 ActiveProducerIndex = std::numeric_limits<uint32>::max();
		uint32 NextProducerIndex = 0;

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
