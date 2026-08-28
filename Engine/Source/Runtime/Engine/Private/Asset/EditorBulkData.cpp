#include "Asset/EditorBulkData.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024 * 1024;
	}

	FEditorBulkData::FEditorBulkData(FGuid PayloadId)
	{
		ReplaceBytes(PayloadId, {});
	}

	auto FEditorBulkData::ReplaceBytes(std::span<const std::byte> Bytes) -> bool
	{
		return ReplaceBytes(Data.GetDescriptor().PayloadId, Bytes);
	}

	auto FEditorBulkData::ReplaceBytes(
		FGuid PayloadId, std::span<const std::byte> Bytes) -> bool
	{
		if (Bytes.size() > MaximumAuthoredBulkBytes || !PayloadId.IsValid())
			return false;
		FBulkDataDescriptor Candidate{
			.PayloadId = PayloadId,
			.LogicalByteCount = static_cast<uint64>(Bytes.size()),
			.ContentHash = FXxHash128::HashBuffer(Bytes)};
		FBulkData CandidateData;
		if (!FBulkData::TryCreate(std::move(Candidate),
				FSharedByteBuffer::Copy(Bytes), CandidateData))
			return false;
		Data = std::move(CandidateData);
		return true;
	}

	auto FEditorBulkData::Serialize(FArchive& Ar) -> void
	{
		const FBulkDataDescriptor& Descriptor = Data.GetDescriptor();
		FArchiveBulkDataTransfer Transfer{
			.PayloadId = Descriptor.PayloadId,
			.LogicalSize = Descriptor.LogicalByteCount,
			.StoredSize = Descriptor.LogicalByteCount,
			.ContentHash = Descriptor.ContentHash,
			.Buffer = Data.GetBuffer()};
		Ar.SerializeBulkData(Transfer);
		if (!Ar.IsLoading() || Ar.HasError()
			|| Ar.GetBulkDataPolicy() == EArchiveBulkDataPolicy::Skip) return;
		FBulkDataDescriptor Candidate{
			.PayloadId = Transfer.PayloadId,
			.LogicalByteCount = Transfer.LogicalSize,
			.ContentHash = Transfer.ContentHash};
		std::string Error;
		if (Transfer.LogicalSize > MaximumAuthoredBulkBytes
			|| Transfer.StoredSize != Transfer.LogicalSize)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Authored bulk bytes exceed the 1 GiB limit or use an unsupported stored size.");
			return;
		}
		FBulkData CandidateData;
		if (!FBulkData::TryCreate(std::move(Candidate),
				std::move(Transfer.Buffer), CandidateData, &Error))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				Error.empty() ? "Loaded authored bulk data is not verified and resident." : Error);
			return;
		}
		Data = std::move(CandidateData);
	}

	auto FEditorBulkData::Identical(const FEditorBulkData& Other) const -> bool
	{
		if (Data.GetDescriptor().LogicalByteCount
				!= Other.Data.GetDescriptor().LogicalByteCount
			|| Data.GetDescriptor().ContentHash
				!= Other.Data.GetDescriptor().ContentHash)
			return false;
		return true;
	}
}
