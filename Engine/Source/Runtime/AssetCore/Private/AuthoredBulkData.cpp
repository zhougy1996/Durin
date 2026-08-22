#include "Asset/AuthoredBulkData.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024 * 1024;

		auto ToBulkDescriptor(const FAuthoredBulkDataDescriptor& Descriptor)
			-> FBulkDataDescriptor
		{
			return {
				.PayloadId = Descriptor.PayloadId,
				.FormatId = Descriptor.FormatId,
				.FormatVersion = Descriptor.FormatVersion,
				.LogicalByteCount = Descriptor.LogicalByteCount,
				.StoredByteCount = Descriptor.StoredByteCount,
				.ContentHash = Descriptor.ContentHash};
		}

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
		FBulkData CandidateData;
		if (!FBulkData::TryCreateResident(ToBulkDescriptor(Candidate),
				FSharedByteBuffer::Copy(Bytes), EBulkDataStorageDomain::Authored,
				CandidateData))
			return false;
		Descriptor = Candidate;
		Data = std::move(CandidateData);
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
			.Buffer = Data.GetResidentBuffer()};
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
		FBulkData CandidateData;
		if (!FBulkData::TryCreateResident(ToBulkDescriptor(Candidate),
				std::move(Transfer.Buffer), EBulkDataStorageDomain::Authored,
				CandidateData, &Error))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				Error.empty() ? "Loaded authored bulk data is not verified and resident." : Error);
			return;
		}
		Descriptor = Candidate;
		Data = std::move(CandidateData);
	}

	auto FAuthoredBulkData::Identical(const FAuthoredBulkData& Other) const -> bool
	{
		if (Descriptor.FormatId != Other.Descriptor.FormatId
			|| Descriptor.FormatVersion != Other.Descriptor.FormatVersion
			|| Descriptor.LogicalByteCount != Other.Descriptor.LogicalByteCount
			|| Descriptor.ContentHash != Other.Descriptor.ContentHash)
			return false;
		return Data.IsResident() && Other.Data.IsResident();
	}
}
