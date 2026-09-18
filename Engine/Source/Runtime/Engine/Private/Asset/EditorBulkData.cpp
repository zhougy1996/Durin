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
		: State(std::atomic_load_explicit(&Other.State, std::memory_order_acquire))
	{
	}

	auto FEditorBulkData::operator=(const FEditorBulkData& Other) -> FEditorBulkData&
	{
		if (this != &Other)
			std::atomic_store_explicit(&State,
				std::atomic_load_explicit(&Other.State, std::memory_order_acquire),
				std::memory_order_release);
		return *this;
	}

	FEditorBulkData::FEditorBulkData(FEditorBulkData&& Other) noexcept
		: State(std::atomic_exchange_explicit(
			&Other.State, MakeEmptyState(), std::memory_order_acq_rel))
	{
	}

	auto FEditorBulkData::operator=(FEditorBulkData&& Other) noexcept -> FEditorBulkData&
	{
		if (this != &Other)
			std::atomic_store_explicit(&State,
				std::atomic_exchange_explicit(
					&Other.State, MakeEmptyState(), std::memory_order_acq_rel),
				std::memory_order_release);
		return *this;
	}

	auto FEditorBulkData::GetInstanceId() const -> FGuid
	{
		return std::atomic_load_explicit(&State, std::memory_order_acquire)->InstanceId;
	}

	auto FEditorBulkData::GetPayloadId() const -> FXxHash128
	{
		return std::atomic_load_explicit(&State, std::memory_order_acquire)->ContentId;
	}

	auto FEditorBulkData::GetPayloadSize() const -> uint64
	{
		return std::atomic_load_explicit(&State, std::memory_order_acquire)->LogicalSize;
	}

	auto FEditorBulkData::IsMemoryResident() const -> bool
	{
		const auto Snapshot = std::atomic_load_explicit(&State, std::memory_order_acquire);
		return std::holds_alternative<FSharedByteBuffer>(Snapshot->Source);
	}

	auto FEditorBulkData::GetPayload() const -> FPackageResourceRequest
	{
		return RequestPayload(std::atomic_load_explicit(&State, std::memory_order_acquire));
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
		auto Expected = std::atomic_load_explicit(&State, std::memory_order_acquire);
		while (true)
		{
			FGuid InstanceId = Expected->InstanceId;
			if (!InstanceId.IsValid()) InstanceId = FGuid::NewGuid();
			const auto Candidate = MakeMemoryState(InstanceId, CandidateId, Buffer);
			if (std::atomic_compare_exchange_weak_explicit(&State, &Expected, Candidate,
				std::memory_order_release, std::memory_order_acquire)) return {};
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
		std::atomic_store_explicit(&OutValue.State, std::make_shared<const FState>(FState{
			.InstanceId = InInstanceId,
			.ContentId = InContentId,
			.LogicalSize = InLogicalSize,
			.Source = std::move(InSource)}), std::memory_order_release);
		return {};
	}

	auto FEditorBulkData::Serialize(FArchive& Ar) -> void
	{
		const auto Snapshot = std::atomic_load_explicit(&State, std::memory_order_acquire);
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
				std::atomic_compare_exchange_strong_explicit(&State, &Expected, Resident,
					std::memory_order_release, std::memory_order_acquire);
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
			std::atomic_store_explicit(&State,
				std::atomic_load_explicit(&Candidate.State, std::memory_order_acquire),
				std::memory_order_release);
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
		std::atomic_store_explicit(&State, MakeMemoryState(
			Value.PayloadId, Value.ContentHash, std::move(Value.Buffer)), std::memory_order_release);
	}

	auto FEditorBulkData::Identical(const FEditorBulkData& Other) const -> bool
	{
		const auto Left = std::atomic_load_explicit(&State, std::memory_order_acquire);
		const auto Right = std::atomic_load_explicit(&Other.State, std::memory_order_acquire);
		return Left->LogicalSize == Right->LogicalSize && Left->ContentId == Right->ContentId;
	}
}
