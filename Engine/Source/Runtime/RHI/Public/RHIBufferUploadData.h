#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Shared across CPU-authored snapshots and owned native upload sources.
	class FRHIBufferUploadAccounting final
	{
	public:
		RHI_API static auto Track(uint64 Bytes)
			-> std::shared_ptr<const FRHIBufferUploadAccounting>;
		RHI_API ~FRHIBufferUploadAccounting();
		FRHIBufferUploadAccounting(const FRHIBufferUploadAccounting&) = delete;
		auto operator=(const FRHIBufferUploadAccounting&) -> FRHIBufferUploadAccounting& = delete;
		auto GetBytes() const -> uint64 { return Bytes; }
	private:
		explicit FRHIBufferUploadAccounting(uint64 InBytes) : Bytes(InBytes) {}
		uint64 Bytes;
	};

	// Immutable CPU payload, not an RHI resource. Sharing it never duplicates bytes.
	class FRHIBufferUploadData final
	{
	public:
		RHI_API static auto Copy(FByteView Data)
			-> std::shared_ptr<const FRHIBufferUploadData>;
		RHI_API static auto Take(FByteBuffer Data)
			-> std::shared_ptr<const FRHIBufferUploadData>;
		auto GetData() const -> FByteView
		{ return CopiedBytes ? FByteView(CopiedBytes.get(), Size) : FByteView(Owned); }
		auto GetOwnedPayloadBytes() const -> uint64 { return Accounting->GetBytes(); }
	private:
		FRHIBufferUploadData() = default;
		// Declared first so storage is freed before its accounting owner.
		std::shared_ptr<const FRHIBufferUploadAccounting> Accounting;
		FByteBuffer Owned;
		std::unique_ptr<std::byte[]> CopiedBytes;
		size_t Size = 0;
	};
}
