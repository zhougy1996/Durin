#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Immutable after publication. Retaining a snapshot retains its bytes and sidecars.
	class FRHIDeferredBufferSnapshot final
	{
	public:
		RHI_API ~FRHIDeferredBufferSnapshot();
		auto GetData() const -> FByteView { return {Data.get(), Size}; }
		auto GetReferences() const -> std::span<const TRefCountPtr<FRHIResource>>
		{ return {References.get(), ReferenceCount}; }
		auto GetVersion() const -> uint64 { return Version; }
		auto GetOwnedPayloadBytes() const -> uint64 { return OwnedBytes; }

	private:
		friend class FRHICommandListBase;
		friend class FRHIBuffer;
		friend class FRHIDeferredBufferBackend;
		static auto TryAllocate(uint32 Size, std::span<FRHIResource* const> References)
			-> std::expected<std::shared_ptr<FRHIDeferredBufferSnapshot>, ERHIBufferUploadError>;
		FRHIDeferredBufferSnapshot(uint32 InSize, size_t InReferenceCount);
		std::unique_ptr<std::byte[]> Data;
		std::unique_ptr<TRefCountPtr<FRHIResource>[]> References;
		uint32 Size;
		size_t ReferenceCount;
		uint64 Version = 0;
		uint64 OwnedBytes = 0;
		mutable std::unordered_map<const void*, std::shared_ptr<void>> Backings;
	};

	class FRHIDeferredBufferBackend final
	{
	public:
		// Only ordered RHI replay may observe the current content version.
		RHI_API static auto ResolveSnapshot(const FRHIBuffer& Buffer)
			-> std::shared_ptr<const FRHIDeferredBufferSnapshot>;
		// Replay-owned cache, partitioned by queue context to preserve exclusive ownership.
		RHI_API static auto GetBacking(const FRHIDeferredBufferSnapshot& Snapshot,
			const void* Context) -> std::shared_ptr<void>;
		RHI_API static auto SetBacking(const FRHIDeferredBufferSnapshot& Snapshot,
			const void* Context, std::shared_ptr<void> Backing) -> void;
	};
}
