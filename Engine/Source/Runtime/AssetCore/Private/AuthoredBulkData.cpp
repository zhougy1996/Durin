#include "Asset/AuthoredBulkData.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024 * 1024;

		class FAuthoredBulkDataProvider final : public IBulkDataProvider
		{
		public:
			explicit FAuthoredBulkDataProvider(FAuthoredBulkData::FLoadFunction InLoader)
				: Loader(std::move(InLoader))
			{
			}

			auto GetStorageDomain() const -> EBulkDataStorageDomain override
			{
				return EBulkDataStorageDomain::Authored;
			}

			auto LoadSynchronous(
				const FBulkDataDescriptor&,
				FSharedByteBuffer& OutBuffer,
				std::string& OutError) const -> bool override
			{
				return Loader(OutBuffer, OutError);
			}

		private:
			FAuthoredBulkData::FLoadFunction Loader;
		};

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
		FBulkData CandidateData;
		if (!FBulkData::TryCreateUnloaded(ToBulkDescriptor(InDescriptor),
				std::make_shared<FAuthoredBulkDataProvider>(std::move(InLoader)),
				CandidateData, &Error))
		{
			if (OutError) *OutError = Error;
			return false;
		}
		Descriptor = InDescriptor;
		Data = std::move(CandidateData);
		if (OutError) OutError->clear();
		return true;
	}

	auto FAuthoredBulkData::LoadSynchronous(std::string& OutError) -> bool
	{
		return Data.LoadSynchronous(OutError);
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
			.Residency = Data.GetResidency() == EBulkDataResidency::Resident
				? EArchiveBulkDataResidency::Resident
				: Data.GetResidency() == EBulkDataResidency::Unloaded
				? EArchiveBulkDataResidency::Unloaded
				: EArchiveBulkDataResidency::Failed,
			.Buffer = Data.GetResidentBuffer(),
			.Failure = std::string(Data.GetFailure())};
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
		if (Transfer.Residency != EArchiveBulkDataResidency::Resident
			|| !FBulkData::TryCreateResident(ToBulkDescriptor(Candidate),
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
		return IsResident() && Other.IsResident();
	}
}
