#include "Asset/EditorBulkData.h"

namespace Durin::Asset
{
	namespace Private
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
		using FState = Private::FEditorBulkDataState;

		auto MakeEmptyState() -> std::shared_ptr<const FState>
		{
			static const auto Empty = std::make_shared<const FState>(FState{
				.ContentId = FXxHash128::HashBuffer(std::span<const std::byte>{}),
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

		auto ErrorResult(EPackageResourceReadStatus Status, std::string Message)
			-> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed({
				.Status = Status, .Message = std::move(Message)});
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
					"Editor bulk payload has no memory or package source.");
			return FPackageResourceRequest::Transform(
				Range.Resource->ReadRangeAsync(Range.SegmentOffset, Range.StoredSize),
				[ExpectedSize = Snapshot->LogicalSize, ExpectedId = Snapshot->ContentId](
					FPackageResourceReadResult Result) {
					if (Result && (Result.Buffer.GetSize() != ExpectedSize
						|| FXxHash128::HashBuffer(Result.Buffer.GetBytes()) != ExpectedId))
						return FPackageResourceReadResult{
							.Status = EPackageResourceReadStatus::SegmentDigestMismatch,
							.Message = "Editor bulk package range does not match its content identity."};
					return Result;
				});
		}
	}

	FEditorBulkData::FEditorBulkData() : State(MakeEmptyState()) {}

	FEditorBulkData::FEditorBulkData(FGuid InInstanceId)
		: State(MakeMemoryState(InInstanceId,
			FXxHash128::HashBuffer(std::span<const std::byte>{}), FSharedByteBuffer{}))
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

	auto FEditorBulkData::UpdatePayload(std::span<const std::byte> Bytes) -> bool
	{
		return UpdatePayload(FSharedByteBuffer::Copy(Bytes));
	}

	auto FEditorBulkData::UpdatePayload(FSharedByteBuffer Buffer) -> bool
	{
		if (Buffer.GetSize() > MaximumAuthoredBulkBytes) return false;
		const FXxHash128 CandidateId = FXxHash128::HashBuffer(Buffer.GetBytes());
		auto Expected = std::atomic_load_explicit(&State, std::memory_order_acquire);
		while (true)
		{
			FGuid InstanceId = Expected->InstanceId;
			if (!InstanceId.IsValid()) InstanceId = FGuid::NewGuid();
			const auto Candidate = MakeMemoryState(InstanceId, CandidateId, Buffer);
			if (std::atomic_compare_exchange_weak_explicit(&State, &Expected, Candidate,
				std::memory_order_release, std::memory_order_acquire)) return true;
		}
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
			|| InLogicalSize != InSource.StoredSize
			|| !ValidatePackageResourceRange(InSource, MaximumAuthoredBulkBytes, OutError))
		{
			if (OutError && OutError->empty())
				*OutError = "Editor bulk package source identity or logical size is invalid.";
			return false;
		}
		std::atomic_store_explicit(&OutValue.State, std::make_shared<const FState>(FState{
			.InstanceId = InInstanceId,
			.ContentId = InContentId,
			.LogicalSize = InLogicalSize,
			.Source = std::move(InSource)}), std::memory_order_release);
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
		if (!LegacyInstanceId.IsValid() || Bytes.size() > MaximumAuthoredBulkBytes) return false;
		const FSharedByteBuffer Buffer = FSharedByteBuffer::Copy(Bytes);
		const FXxHash128 CandidateId = FXxHash128::HashBuffer(Buffer.GetBytes());
		auto Expected = std::atomic_load_explicit(&State, std::memory_order_acquire);
		while (true)
		{
			const FGuid InstanceId = Expected->InstanceId.IsValid()
				? Expected->InstanceId : LegacyInstanceId;
			const auto Candidate = MakeMemoryState(InstanceId, CandidateId, Buffer);
			if (std::atomic_compare_exchange_weak_explicit(&State, &Expected, Candidate,
				std::memory_order_release, std::memory_order_acquire)) return true;
		}
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
					Payload.Message.empty() ? "Authored bulk payload cannot be read for serialization."
						: Payload.Message);
				return;
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
