#include "Asset/EditorBulkData.h"

namespace Durin::Asset
{
	namespace
	{
		auto ErrorResult(EPackageResourceReadStatus Status, std::string Message)
			-> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed({
				.Status = Status, .Message = std::move(Message)});
		}

		auto ValidateSource(
			uint64 LogicalSize,
			const FEditorBulkDataSource& Source,
			std::string* OutError) -> bool
		{
			const bool bValid = Source.Resource && Source.StorageFlags == 0
				&& LogicalSize == Source.StoredSize && LogicalSize <= MaximumAuthoredBulkBytes
				&& Source.Alignment != 0 && Source.Alignment <= 4096
				&& (Source.Alignment & (Source.Alignment - 1)) == 0
				&& Source.SegmentOffset % Source.Alignment == 0
				&& Source.SegmentOffset <= Source.Resource->GetSegmentExtent()
				&& Source.StoredSize <= Source.Resource->GetSegmentExtent() - Source.SegmentOffset;
			if (!bValid)
			{
				if (OutError) *OutError = "Editor bulk package source is invalid or unsupported.";
				return false;
			}
			if (OutError) OutError->clear();
			return true;
		}
	}

	FEditorBulkData::FEditorBulkData()
		: ContentId(FXxHash128::HashBuffer(std::span<const std::byte>{}))
	{
	}

	FEditorBulkData::FEditorBulkData(FGuid InInstanceId)
		: InstanceId(InInstanceId)
		, ContentId(FXxHash128::HashBuffer(std::span<const std::byte>{}))
	{
	}

	auto FEditorBulkData::GetPayload() const -> FPackageResourceRequest
	{
		if (bHasMemory)
			return FPackageResourceRequest::Completed({
				.Status = EPackageResourceReadStatus::Success, .Buffer = Memory});
		if (!Source.Resource)
			return ErrorResult(EPackageResourceReadStatus::MissingSegment,
				"Editor bulk payload has no memory or package source.");
		return FPackageResourceRequest::Transform(
			Source.Resource->ReadRangeAsync(Source.SegmentOffset, Source.StoredSize),
			[ExpectedSize = LogicalSize, ExpectedId = ContentId](FPackageResourceReadResult Result) {
				if (Result && (Result.Buffer.GetSize() != ExpectedSize
					|| FXxHash128::HashBuffer(Result.Buffer.GetBytes()) != ExpectedId))
					return FPackageResourceReadResult{
						.Status = EPackageResourceReadStatus::SegmentDigestMismatch,
						.Message = "Editor bulk package range does not match its content identity."};
				return Result;
			});
	}

	auto FEditorBulkData::UpdatePayload(std::span<const std::byte> Bytes) -> bool
	{
		return UpdatePayload(FSharedByteBuffer::Copy(Bytes));
	}

	auto FEditorBulkData::UpdatePayload(FSharedByteBuffer Buffer) -> bool
	{
		if (Buffer.GetSize() > MaximumAuthoredBulkBytes) return false;
		const FXxHash128 CandidateId = FXxHash128::HashBuffer(Buffer.GetBytes());
		FGuid CandidateInstanceId = InstanceId;
		if (!CandidateInstanceId.IsValid()) CandidateInstanceId = FGuid::NewGuid();
		InstanceId = CandidateInstanceId;
		ContentId = CandidateId;
		LogicalSize = Buffer.GetSize();
		Memory = std::move(Buffer);
		Source = {};
		bHasMemory = true;
		return true;
	}

	auto FEditorBulkData::TryCreatePackageBacked(
		FGuid InInstanceId,
		FXxHash128 InContentId,
		uint64 InLogicalSize,
		FEditorBulkDataSource InSource,
		FEditorBulkData& OutValue,
		std::string* OutError) -> bool
	{
		if (!InInstanceId.IsValid() || InContentId.IsZero()
			|| !ValidateSource(InLogicalSize, InSource, OutError)) return false;
		FEditorBulkData Candidate;
		Candidate.InstanceId = InInstanceId;
		Candidate.ContentId = InContentId;
		Candidate.LogicalSize = InLogicalSize;
		Candidate.Memory = {};
		Candidate.Source = std::move(InSource);
		Candidate.bHasMemory = false;
		OutValue = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FEditorBulkData::ReplaceBytes(std::span<const std::byte> Bytes) -> bool
	{
		return UpdatePayload(Bytes);
	}

	auto FEditorBulkData::ReplaceBytes(
		FGuid LegacyInstanceId, std::span<const std::byte> Bytes) -> bool
	{
		if (!LegacyInstanceId.IsValid()) return false;
		if (!InstanceId.IsValid()) InstanceId = LegacyInstanceId;
		return UpdatePayload(Bytes);
	}

	auto FEditorBulkData::Serialize(FArchive& Ar) -> void
	{
		FPackageResourceReadResult Payload;
		if (Ar.IsSaving() && !Ar.IsDiscovering()
			&& Ar.GetBulkDataPolicy() != EArchiveBulkDataPolicy::Skip)
		{
			Payload = GetPayload().Wait();
			if (!Payload)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Payload.Message.empty() ? "Authored bulk payload cannot be read for serialization."
						: Payload.Message);
				return;
			}
		}
		FArchiveBulkDataValue Value{
			.PayloadId = InstanceId,
			.LogicalSize = LogicalSize,
			.StoredSize = LogicalSize,
			.ContentHash = ContentId,
			.Buffer = Ar.IsSaving() ? Payload.Buffer : FSharedByteBuffer{}};
		Ar.SerializeBulkData(Value, {
			.Owner = this,
			.ElementSize = 1,
			.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
		if (!Ar.IsLoading() || Ar.HasError()
			|| Ar.GetBulkDataPolicy() == EArchiveBulkDataPolicy::Skip) return;
		if (Value.StorageKind == EArchiveBulkDataStorageKind::External)
		{
			FEditorBulkData Candidate;
			std::string Error;
			if (!Value.PackageResource || !TryCreatePackageBacked(
				Value.PayloadId, Value.ContentHash, Value.LogicalSize,
				{.Resource = std::static_pointer_cast<FPackageResource>(Value.PackageResource),
					.SegmentOffset = Value.SegmentOffset,
					.StoredSize = Value.StoredSize,
					.Alignment = Value.Alignment}, Candidate, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Error.empty() ? "Loaded external authored bulk source is invalid." : Error);
				return;
			}
			*this = std::move(Candidate);
			return;
		}
		if (!Value.PayloadId.IsValid() || Value.LogicalSize > MaximumAuthoredBulkBytes
			|| Value.StoredSize != Value.LogicalSize
			|| Value.Buffer.GetSize() != Value.LogicalSize
			|| FXxHash128::HashBuffer(Value.Buffer.GetBytes()) != Value.ContentHash)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Loaded authored bulk data identity, size, or content is invalid.");
			return;
		}
		InstanceId = Value.PayloadId;
		ContentId = Value.ContentHash;
		LogicalSize = Value.LogicalSize;
		Memory = std::move(Value.Buffer);
		Source = {};
		bHasMemory = true;
	}

	auto FEditorBulkData::Identical(const FEditorBulkData& Other) const -> bool
	{
		return LogicalSize == Other.LogicalSize && ContentId == Other.ContentId;
	}
}
