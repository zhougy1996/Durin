#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"
#include "RHIBufferUploadData.h"

namespace Durin
{
	class FRHIStorageBuffer;

	// Storage-only contents, immutable after publication; partial updates preserve replay order.
	class FRHIStorageBufferSnapshot final
	{
	public:
		RHI_API ~FRHIStorageBufferSnapshot();
		auto GetData() const -> FByteView { return {Data.get(), Size}; }
		auto GetVersion() const -> uint64 { return Version; }
		auto GetOwnedPayloadBytes() const -> uint64 { return OwnedBytes; }
		auto GetBackingAdmission() const -> const std::shared_ptr<void>& { return BackingAdmission; }

	private:
		friend class FRHICommandListBase;
		friend class FRHIStorageBuffer;
		friend class FRHIStorageBufferBackend;
		static auto Allocate(const FRHIBufferDesc& Desc)
			-> std::shared_ptr<FRHIStorageBufferSnapshot>;
		FRHIStorageBufferSnapshot(uint32 InSize);
		std::unique_ptr<std::byte[]> Data;
		uint32 Size;
		uint64 Version = 0;
		uint64 OwnedBytes = 0;
		std::shared_ptr<const FRHIBufferUploadAccounting> Accounting;
		std::shared_ptr<void> BackingAdmission;
		mutable std::unordered_map<const void*, std::weak_ptr<void>> Backings;
	};

	class FRHIStorageBufferBackend final
	{
	public:
		RHI_API static auto RecordAdmission(uint64 Bytes, uint64 Capacity, bool bAcquire) -> void;
		// Only ordered RHI replay may observe the current content version.
		RHI_API static auto ResolveSnapshot(const FRHIBuffer& Buffer)
			-> std::shared_ptr<const FRHIStorageBufferSnapshot>;
		// Replay-owned cache, partitioned by queue context to preserve exclusive ownership.
		RHI_API static auto GetBacking(const FRHIStorageBufferSnapshot& Snapshot,
			const void* Context) -> std::shared_ptr<void>;
		RHI_API static auto SetBacking(const FRHIStorageBufferSnapshot& Snapshot,
			const void* Context, std::shared_ptr<void> Backing) -> void;
	};
}
