#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Shared across CPU-authored snapshots and owned native upload sources.
	class FRHIBufferUploadReservation final
	{
	public:
		static constexpr uint64 MaxSingleBytes = 16ull * 1024 * 1024;
		static constexpr uint64 MaxLiveBytes = 32ull * 1024 * 1024;
		RHI_API static auto TryReserve(uint64 Bytes)
			-> std::expected<std::shared_ptr<const FRHIBufferUploadReservation>, ERHIBufferUploadError>;
		RHI_API ~FRHIBufferUploadReservation();
		FRHIBufferUploadReservation(const FRHIBufferUploadReservation&) = delete;
		auto operator=(const FRHIBufferUploadReservation&) -> FRHIBufferUploadReservation& = delete;
		auto GetBytes() const -> uint64 { return Bytes; }
	private:
		explicit FRHIBufferUploadReservation(uint64 InBytes) : Bytes(InBytes) {}
		uint64 Bytes;
	};

	// Immutable CPU payload, not an RHI resource. Sharing it never duplicates bytes.
	class FRHIBufferUploadData final
	{
	public:
		RHI_API static auto TryCopy(FByteView Data)
			-> std::expected<std::shared_ptr<const FRHIBufferUploadData>, ERHIBufferUploadError>;
		RHI_API static auto TryTake(FByteBuffer Data)
			-> std::expected<std::shared_ptr<const FRHIBufferUploadData>, ERHIBufferUploadError>;
		auto GetData() const -> FByteView
		{ return Copy ? FByteView(Copy.get(), Size) : FByteView(Owned); }
		auto GetOwnedPayloadBytes() const -> uint64 { return Reservation->GetBytes(); }
	private:
		FRHIBufferUploadData() = default;
		// Declared first so storage is freed before its reservation.
		std::shared_ptr<const FRHIBufferUploadReservation> Reservation;
		FByteBuffer Owned;
		std::unique_ptr<std::byte[]> Copy;
		size_t Size = 0;
	};
}
