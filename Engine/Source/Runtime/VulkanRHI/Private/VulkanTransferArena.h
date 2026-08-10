#pragma once

#include "CoreMinimal.h"
#include "VulkanCompletion.h"
#include "VulkanDiagnostics.h"
#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	class FVulkanBuffer;
	class FVulkanDevice;
	class FVulkanTransferArena;

	struct FVulkanTransferArenaConfig
	{
		EVulkanAllocationClassCandidate AllocationClass =
			EVulkanAllocationClassCandidate::TransferUpload;
		uint64 PageSize = 0;
		uint32 MaxPageCount = 0;
		const char* DebugName = nullptr;
	};

	// A mapped buffer interval reserved for one queue submission.
	class VULKANRHI_API FVulkanTransferRange
	{
	public:
		FVulkanTransferRange() = default;
		FVulkanTransferRange(FVulkanTransferRange&& Other) noexcept;
		auto operator=(FVulkanTransferRange&& Other) noexcept
			-> FVulkanTransferRange&;
		~FVulkanTransferRange();

		FVulkanTransferRange(const FVulkanTransferRange&) = delete;
		auto operator=(const FVulkanTransferRange&)
			-> FVulkanTransferRange& = delete;

		auto IsValid() const -> bool { return Buffer != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetBuffer() const -> FVulkanBuffer* { return Buffer; }
		auto GetOffset() const -> uint64 { return Offset; }
		auto GetSize() const -> uint64 { return Size; }
		auto GetToken() const -> FVulkanCompletionToken { return Token; }
		auto GetMappedPointer() const -> uint8*;
		auto Flush() const -> void;
		auto Invalidate() const -> void;
		auto Retire() -> void;

	private:
		friend class FVulkanTransferArena;

		FVulkanTransferArena* Owner = nullptr;
		void* Page = nullptr;
		FVulkanBuffer* Buffer = nullptr;
		uint64 Offset = 0;
		uint64 Size = 0;
		FVulkanCompletionToken Token = 0;
		bool bOversize = false;
	};

	struct FVulkanTransferAcquireResult
	{
		FVulkanTransferRange Range;
		FVulkanCompletionToken WaitToken = 0;
		bool bAllocationFailed = false;
	};

	// Owns bounded persistently mapped pages and recycles ranges by exact token.
	class VULKANRHI_API FVulkanTransferArena
	{
	public:
		FVulkanTransferArena(FVulkanDevice& InDevice,
			FVulkanTransferArenaConfig InConfig);
		~FVulkanTransferArena();
		FVulkanTransferArena(const FVulkanTransferArena&) = delete;
		auto operator=(const FVulkanTransferArena&)
			-> FVulkanTransferArena& = delete;

		auto Acquire(uint64 Size, uint64 Alignment,
			FVulkanCompletionToken Token) -> FVulkanTransferAcquireResult;
		auto ReclaimCompleted() -> void;
		auto GetConfig() const -> const FVulkanTransferArenaConfig&
		{
			return Config;
		}
		auto GetPageCount() const -> uint32;

	private:
		friend class FVulkanTransferRange;
		struct FFreeRange
		{
			uint64 Offset = 0;
			uint64 Size = 0;
		};
		struct FRetiredRange
		{
			uint64 Offset = 0;
			uint64 Size = 0;
			FVulkanCompletionToken Token = 0;
		};
		struct FPage
		{
			TRefCountPtr<FVulkanBuffer> Buffer;
			uint64 Size = 0;
			bool bOversize = false;
			bool bHasServedAllocation = false;
			std::vector<FFreeRange> FreeRanges;
			std::vector<FRetiredRange> RetiredRanges;
		};
		auto CreatePage(uint64 Size, bool bOversize) -> FPage*;
		auto TryAllocateFromPage(FPage& Page, uint64 Size, uint64 Alignment,
			FVulkanCompletionToken Token) -> FVulkanTransferRange;
		auto GetOldestRetiredToken() const -> FVulkanCompletionToken;
		auto Cancel(FVulkanTransferRange& Range) -> void;
		auto Retire(FVulkanTransferRange& Range) -> void;
		auto InsertFreeRange(FPage& Page, FFreeRange Range) -> void;
		auto DestroyPage(FPage& Page) -> void;

		FVulkanDevice& Device;
		FVulkanTransferArenaConfig Config;
		std::vector<std::unique_ptr<FPage>> Pages;
		std::vector<std::unique_ptr<FPage>> OversizePages;
	};
}
