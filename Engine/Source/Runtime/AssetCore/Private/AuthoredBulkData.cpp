#include "Asset/AuthoredBulkData.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024 * 1024;

		auto ValidateDescriptor(const FAuthoredBulkDataDescriptor& Descriptor,
			std::string* OutError) -> bool
		{
			if (!Descriptor.PayloadId.IsValid() || !Descriptor.FormatId.IsValid()
				|| Descriptor.FormatVersion == 0)
			{
				if (OutError) *OutError = "Authored bulk identity requires valid payload and format ids plus a nonzero format version.";
				return false;
			}
			if (Descriptor.LogicalByteCount > MaximumAuthoredBulkBytes
				|| Descriptor.StoredByteCount != Descriptor.LogicalByteCount)
			{
				if (OutError) *OutError = "Authored bulk bytes exceed the 1 GiB limit or use an unsupported stored size.";
				return false;
			}
			return true;
		}
	}

	FAuthoredBulkData::FAuthoredBulkData(
		FGuid PayloadId, FGuid FormatId, uint32 FormatVersion)
	{
		ReplaceBytes(PayloadId, FormatId, FormatVersion, {});
	}

	auto FAuthoredBulkData::ReplaceBytes(std::span<const std::byte> Bytes) -> bool
	{
		return ReplaceBytes(
			Descriptor.PayloadId, Descriptor.FormatId, Descriptor.FormatVersion, Bytes);
	}

	auto FAuthoredBulkData::ReplaceBytes(
		FGuid PayloadId, FGuid FormatId, uint32 FormatVersion,
		std::span<const std::byte> Bytes) -> bool
	{
		if (Bytes.size() > MaximumAuthoredBulkBytes || !PayloadId.IsValid()
			|| !FormatId.IsValid() || FormatVersion == 0)
			return false;
		FAuthoredBulkDataDescriptor Candidate{
			.PayloadId = PayloadId,
			.FormatId = FormatId,
			.FormatVersion = FormatVersion,
			.LogicalByteCount = static_cast<uint64>(Bytes.size()),
			.StoredByteCount = static_cast<uint64>(Bytes.size()),
			.ContentHash = FXxHash128::HashBuffer(Bytes),
			.ContainerHash = {},
			.StorageKind = EAuthoredBulkStorageKind::Inline};
		Descriptor = Candidate;
		Buffer = FSharedByteBuffer::Copy(Bytes);
		Residency = EArchiveBulkDataResidency::Resident;
		Loader = {};
		Failure.clear();
		bHashVerified = true;
		return true;
	}

	auto FAuthoredBulkData::SetUnloaded(
		FAuthoredBulkDataDescriptor InDescriptor, FLoadFunction InLoader,
		std::string* OutError) -> bool
	{
		std::string Error;
		if (!ValidateDescriptor(InDescriptor, &Error)
			|| InDescriptor.StorageKind != EAuthoredBulkStorageKind::External
			|| InDescriptor.ContainerHash.IsZero()
			|| !InLoader)
		{
			if (Error.empty()) Error = "Unloaded authored bulk data requires an external descriptor and loader.";
			if (OutError) *OutError = Error;
			return false;
		}
		Descriptor = InDescriptor;
		Residency = EArchiveBulkDataResidency::Unloaded;
		Buffer = {};
		Loader = std::move(InLoader);
		Failure.clear();
		bHashVerified = false;
		if (OutError) OutError->clear();
		return true;
	}

	auto FAuthoredBulkData::LoadSynchronous(std::string& OutError) -> bool
	{
		if (Residency == EArchiveBulkDataResidency::Resident)
		{
			OutError.clear();
			return true;
		}
		if (!Loader)
		{
			OutError = Failure.empty() ? "Authored bulk payload has no synchronous loader." : Failure;
			return false;
		}
		FSharedByteBuffer Candidate;
		std::string CandidateError;
		if (!Loader(Candidate, CandidateError)
			|| Candidate.GetSize() != Descriptor.LogicalByteCount
			|| FXxHash128::HashBuffer(Candidate.GetBytes()) != Descriptor.ContentHash)
		{
			Failure = CandidateError.empty()
				? "Authored bulk payload size or hash verification failed."
				: std::move(CandidateError);
			Residency = EArchiveBulkDataResidency::Failed;
			OutError = Failure;
			return false;
		}
		Buffer = std::move(Candidate);
		Residency = EArchiveBulkDataResidency::Resident;
		Failure.clear();
		bHashVerified = true;
		OutError.clear();
		return true;
	}

	auto FAuthoredBulkData::Serialize(FArchive& Ar) -> void
	{
		FArchiveBulkDataTransfer Transfer{
			.PayloadId = Descriptor.PayloadId,
			.FormatId = Descriptor.FormatId,
			.FormatVersion = Descriptor.FormatVersion,
			.LogicalSize = Descriptor.LogicalByteCount,
			.StoredSize = Descriptor.StoredByteCount,
			.ContentHash = Descriptor.ContentHash,
			.ContainerHash = Descriptor.ContainerHash,
			.StorageKind = Descriptor.StorageKind == EAuthoredBulkStorageKind::Inline
				? EArchiveBulkDataStorageKind::Inline : EArchiveBulkDataStorageKind::External,
			.Residency = Residency,
			.Buffer = Buffer,
			.Failure = Failure};
		Ar.SerializeBulkData(Transfer);
		if (!Ar.IsLoading() || Ar.HasError()
			|| Ar.GetBulkDataPolicy() == EArchiveBulkDataPolicy::Skip) return;
		FAuthoredBulkDataDescriptor Candidate{
			.PayloadId = Transfer.PayloadId,
			.FormatId = Transfer.FormatId,
			.FormatVersion = Transfer.FormatVersion,
			.LogicalByteCount = Transfer.LogicalSize,
			.StoredByteCount = Transfer.StoredSize,
			.ContentHash = Transfer.ContentHash,
			.ContainerHash = Transfer.ContainerHash,
			.StorageKind = Transfer.StorageKind == EArchiveBulkDataStorageKind::Inline
				? EAuthoredBulkStorageKind::Inline : EAuthoredBulkStorageKind::External};
		std::string Error;
		if (!ValidateDescriptor(Candidate, &Error))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, Error);
			return;
		}
		Descriptor = Candidate;
		Residency = Transfer.Residency;
		Buffer = std::move(Transfer.Buffer);
		Failure = std::move(Transfer.Failure);
		Loader = {};
		bHashVerified = Residency == EArchiveBulkDataResidency::Resident;
	}

	auto FAuthoredBulkData::Identical(const FAuthoredBulkData& Other) const -> bool
	{
		if (Descriptor.FormatId != Other.Descriptor.FormatId
			|| Descriptor.FormatVersion != Other.Descriptor.FormatVersion
			|| Descriptor.LogicalByteCount != Other.Descriptor.LogicalByteCount
			|| Descriptor.ContentHash != Other.Descriptor.ContentHash)
			return false;
		if (bHashVerified && Other.bHashVerified) return true;
		return IsResident() && Other.IsResident()
			&& std::ranges::equal(Buffer.GetBytes(), Other.Buffer.GetBytes());
	}
}
