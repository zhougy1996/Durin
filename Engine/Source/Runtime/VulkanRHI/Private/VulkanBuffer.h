#pragma once

#include "RHIResources.h"
#include "VulkanMemory.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanBuffer : public FRHIBuffer
	{
	public:
		FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc);

		~FVulkanBuffer() override;

		auto Lock(const FRHICommandListImmediate& RHICmdList, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*;

		auto Unlock(const FRHICommandListImmediate& RHICmdList) -> void;

		auto IsDynamic() const -> bool;

		auto IsStatic() const -> bool;

		auto GetHandle() const -> vk::Buffer { return Buffer; }

		auto GetMappedPointer() const -> void*;

		auto FlushMappedMemory(uint32 Offset = 0, uint32 Size = 0) -> void;

	protected:
		enum class ELockStatus : uint8
		{
			Unlocked,
			Locked,
			PersistentMapping,
		};

		FVulkanDevice& Device;

		vk::Buffer Buffer{};

		FVulkanAllocation Allocation{};

		ELockStatus LockStatus = ELockStatus::Unlocked;
	};

	class FVulkanDynamicUniformBufferAllocator
	{
	public:
		explicit FVulkanDynamicUniformBufferAllocator(FVulkanDevice& InDevice);

		~FVulkanDynamicUniformBufferAllocator();

		auto BeginFrame(uint32 FrameIndex) -> void;

		auto Allocate(const void* Data, uint32 Size) -> FRHIUniformBufferRange;

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
		};

		auto CreateChunk(uint32 MinSize) -> FChunk;

		auto GetAlignment() const -> uint32;

		static auto AlignUp(uint32 Value, uint32 Alignment) -> uint32;

		FVulkanDevice& Device;

		std::array<FFrameState, kFrameInFlight> Frames;

		uint32 CurrentFrameIndex = 0;
	};

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
