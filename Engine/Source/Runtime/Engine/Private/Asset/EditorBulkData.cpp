#include "Asset/EditorBulkData.h"
#include "EditorBulkDataSaveRetention.h"

namespace Durin
{
	namespace AssetPrivate
	{
		// Publishes authored identity, size, and exactly one immutable source as one state.
		struct FEditorBulkDataState
		{
			FGuid InstanceId;
			FXxHash128 ContentId;
			uint64 LogicalSize = 0;
			std::variant<FSharedByteBuffer, FPackageResourceRange> Source;
		};
	}

	namespace
	{
		using FState = AssetPrivate::FEditorBulkDataState;

		auto MakeEmptyState() -> std::shared_ptr<const FState>
		{
			static const auto Empty = std::make_shared<const FState>(FState{
				.ContentId = FXxHash128::HashBuffer(FByteView{}),
				.Source = FSharedByteBuffer{}});
			return Empty;
		}

		auto MakeMemoryState(
			FGuid InstanceId,
			FXxHash128 ContentId,
			FSharedByteBuffer Buffer) -> std::shared_ptr<const FState>
		{
			return std::make_shared<const FState>(FState{
				.InstanceId = InstanceId,
				.ContentId = ContentId,
				.LogicalSize = Buffer.GetSize(),
				.Source = std::move(Buffer)});
		}

		auto ErrorResult(EPackageResourceReadStatus Status, EPackageResourceReadReason Reason)
			-> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed({
				.Status = Status, .Error = {.Reason = Reason}});
		}

		auto RequestPayload(const std::shared_ptr<const FState>& Snapshot)
			-> FPackageResourceRequest
		{
			if (const auto* Memory = std::get_if<FSharedByteBuffer>(&Snapshot->Source))
				return FPackageResourceRequest::Completed({
					.Status = EPackageResourceReadStatus::Success, .Buffer = *Memory});
			const FPackageResourceRange& Range = std::get<FPackageResourceRange>(Snapshot->Source);
			if (!Range.Resource)
				return ErrorResult(EPackageResourceReadStatus::MissingSegment,
					EPackageResourceReadReason::MissingSource);
			return FPackageResourceRequest::Transform(
				Range.Resource->ReadRangeAsync(Range.SegmentOffset, Range.StoredSize),
				[ExpectedSize = Snapshot->LogicalSize, ExpectedId = Snapshot->ContentId](
					FPackageResourceReadResult Result) {
					if (Result)
					{
						const auto Digest = FXxHash128::HashBuffer(Result.Buffer.GetBytes());
						if (Result.Buffer.GetSize() != ExpectedSize || Digest != ExpectedId)
							return FPackageResourceReadResult{.Status = EPackageResourceReadStatus::SegmentDigestMismatch,
								.Error = {.Reason = EPackageResourceReadReason::ContentMismatch,
									.Actual = Result.Buffer.GetSize(), .Expected = ExpectedSize,
									.ActualDigest = Digest, .ExpectedDigest = ExpectedId}};
					}
					return Result;
				});
		}
	}

	auto FormatEditorBulkDataError(const FEditorBulkDataError& Error) -> std::string
	{
		if (Error.RangeCause) return FormatPackageResourceRangeError(*Error.RangeCause);
		if (Error.Code == EEditorBulkDataError::None) return {};
		if (Error.Code == EEditorBulkDataError::PayloadSizeLimit)
			return "Editor bulk payload exceeds the 1 GiB authored limit.";
		return "Editor bulk package source identity or logical size is invalid.";
	}

	FEditorBulkData::FEditorBulkData() : State(MakeEmptyState()) {}

	FEditorBulkData::FEditorBulkData(FGuid InInstanceId)
		: State(MakeMemoryState(InInstanceId,
			FXxHash128::HashBuffer(FByteView{}), FSharedByteBuffer{}))
	{
	}

	FEditorBulkData::FEditorBulkData(const FEditorBulkData& Other)
		: State(Other.State.Load())
	{
	}

	auto FEditorBulkData::operator=(const FEditorBulkData& Other) -> FEditorBulkData&
	{
		if (this != &Other)
			State.Store(Other.State.Load());
		return *this;
	}

	FEditorBulkData::FEditorBulkData(FEditorBulkData&& Other) noexcept
		: State(Other.State.Exchange(MakeEmptyState()))
	{
	}

	auto FEditorBulkData::operator=(FEditorBulkData&& Other) noexcept -> FEditorBulkData&
	{
		if (this != &Other)
			State.Store(Other.State.Exchange(MakeEmptyState()));
		return *this;
	}

	auto FEditorBulkData::GetInstanceId() const -> FGuid
	{
		return State.Load()->InstanceId;
	}

	auto FEditorBulkData::GetPayloadId() const -> FXxHash128
	{
		return State.Load()->ContentId;
	}

	auto FEditorBulkData::GetPayloadSize() const -> uint64
	{
		return State.Load()->LogicalSize;
	}

	auto FEditorBulkData::IsMemoryResident() const -> bool
	{
		const auto Snapshot = State.Load();
		return std::holds_alternative<FSharedByteBuffer>(Snapshot->Source);
	}

	auto FEditorBulkData::GetPayload() const -> FPackageResourceRequest
	{
		return RequestPayload(State.Load());
	}

	auto FEditorBulkData::UpdatePayload(FByteView Bytes) -> FEditorBulkDataResult
	{
		return UpdatePayload(FSharedByteBuffer::Copy(Bytes));
	}

	auto FEditorBulkData::UpdatePayload(FSharedByteBuffer Buffer) -> FEditorBulkDataResult
	{
		if (Buffer.GetSize() > MaximumAuthoredBulkBytes)
			return {.Error = {.Code = EEditorBulkDataError::PayloadSizeLimit,
				.Actual = Buffer.GetSize(), .Expected = MaximumAuthoredBulkBytes}};
		const FXxHash128 CandidateId = FXxHash128::HashBuffer(Buffer.GetBytes());
		auto Expected = State.Load();
		while (true)
		{
			FGuid InstanceId = Expected->InstanceId;
			if (!InstanceId.IsValid()) InstanceId = FGuid::NewGuid();
			const auto Candidate = MakeMemoryState(InstanceId, CandidateId, Buffer);
			if (State.CompareExchange(Expected, Candidate)) return {};
		}
	}

	auto FEditorBulkData::TryCreatePackageBacked(
		FGuid InInstanceId,
		FXxHash128 InContentId,
		uint64 InLogicalSize,
		FEditorBulkDataSource InSource,
		FEditorBulkData& OutValue) -> FEditorBulkDataResult
	{
		auto Reject = [&](EEditorBulkDataError Code) {
			return FEditorBulkDataResult{.Error = {.Code = Code,
				.InstanceId = InInstanceId, .ContentId = InContentId,
				.Actual = InLogicalSize, .Expected = InSource.StoredSize}};
		};
		if (!InInstanceId.IsValid()) return Reject(EEditorBulkDataError::InvalidInstanceIdentity);
		if (InContentId.IsZero()) return Reject(EEditorBulkDataError::MissingContentIdentity);
		if (InLogicalSize != InSource.StoredSize) return Reject(EEditorBulkDataError::LogicalSizeMismatch);
		if (const auto Validation = ValidatePackageResourceRange(InSource, MaximumAuthoredBulkBytes); !Validation)
		{
			auto Result = Reject(EEditorBulkDataError::InvalidRange);
			Result.Error.RangeCause = Validation.Error;
			return Result;
		}
		OutValue.State.Store(std::make_shared<const FState>(FState{
			.InstanceId = InInstanceId,
			.ContentId = InContentId,
			.LogicalSize = InLogicalSize,
			.Source = std::move(InSource)}));
		return {};
	}

	auto FEditorBulkData::Serialize(FArchive& Ar) -> void
	{
		const auto Snapshot = State.Load();
		FPackageResourceReadResult Payload;
		if (Ar.IsSaving() && !Ar.IsDiscovering()
			&& Ar.GetBulkDataPolicy() != EArchiveBulkDataPolicy::Skip)
		{
			Payload = RequestPayload(Snapshot).Wait();
			if (!Payload)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					FormatPackageResourceReadError(Payload));
				return;
			}
			if (AssetPrivate::FScopedBulkSaveRetention::IsEnabled())
			{
				auto Expected = Snapshot;
				const auto Resident = MakeMemoryState(Snapshot->InstanceId, Snapshot->ContentId, Payload.Buffer);
				State.CompareExchange(Expected, Resident);
			}
		}
		FArchiveBulkDataValue Value{
			.PayloadId = Snapshot->InstanceId,
			.LogicalSize = Snapshot->LogicalSize,
			.StoredSize = Snapshot->LogicalSize,
			.ContentHash = Snapshot->ContentId,
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
			const auto Created = TryCreatePackageBacked(
				Value.PayloadId, Value.ContentHash, Value.LogicalSize,
				{.Resource = std::static_pointer_cast<FPackageResource>(Value.PackageResource),
					.SegmentOffset = Value.SegmentOffset,
					.StoredSize = Value.StoredSize,
					.Alignment = Value.Alignment}, Candidate);
			if (!Created)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					FormatEditorBulkDataError(Created.Error));
				return;
			}
			State.Store(Candidate.State.Load());
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
		State.Store(MakeMemoryState(
			Value.PayloadId, Value.ContentHash, std::move(Value.Buffer)));
	}

	auto FEditorBulkData::Identical(const FEditorBulkData& Other) const -> bool
	{
		const auto Left = State.Load();
		const auto Right = Other.State.Load();
		return Left->LogicalSize == Right->LogicalSize && Left->ContentId == Right->ContentId;
	}
}
