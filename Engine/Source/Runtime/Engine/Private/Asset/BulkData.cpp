#include "Asset/BulkData.h"

namespace Durin
{
	namespace AssetPrivate
	{
		struct FBulkDataState
		{
			mutable std::mutex Mutex;
			FBulkDataMetadata Metadata;
			std::shared_ptr<FByteBuffer> Allocation;
			EBulkDataState State = EBulkDataState::Empty;
			uint32 ReadLocks = 0;
		};
	}

	namespace
	{
		auto BulkDataFail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto NewEmptyState() -> std::shared_ptr<AssetPrivate::FBulkDataState>
		{
			return std::make_shared<AssetPrivate::FBulkDataState>();
		}

		auto ValidateMetadata(const FBulkDataMetadata& Metadata, std::string* OutError) -> bool
		{
			if (Metadata.LogicalSize != Metadata.Range.StoredSize
				|| Metadata.LogicalSize > MaximumBulkDataBytes
				|| !ValidatePackageResourceRange(
					Metadata.Range, MaximumBulkDataBytes, OutError))
				return BulkDataFail("Bulk data package metadata is invalid or unsupported.", OutError);
			if (OutError) OutError->clear();
			return true;
		}

		auto Snapshot(const std::shared_ptr<AssetPrivate::FBulkDataState>& Source)
			-> std::shared_ptr<AssetPrivate::FBulkDataState>
		{
			auto Result = NewEmptyState();
			std::lock_guard Lock(Source->Mutex);
			Result->Metadata = Source->Metadata;
			Result->Allocation = Source->Allocation;
			if (Source->State == EBulkDataState::Empty) Result->State = EBulkDataState::Empty;
			else if (Source->State == EBulkDataState::Detached || !Source->Metadata.Range.Resource)
				Result->State = EBulkDataState::Detached;
			else if (Source->Allocation) Result->State = EBulkDataState::Resident;
			else if (Source->Metadata.Range.Resource->IsRetired()) Result->State = EBulkDataState::Retired;
			else Result->State = EBulkDataState::Attached;
			return Result;
		}
	}

	FBulkData::FBulkData() : State(NewEmptyState()) {}
	FBulkData::~FBulkData() = default;
	FBulkData::FBulkData(const FBulkData& Other) : State(Snapshot(Other.State)) {}

	auto FBulkData::operator=(const FBulkData& Other) -> FBulkData&
	{
		if (this != &Other) State = Snapshot(Other.State);
		return *this;
	}

	FBulkData::FBulkData(FBulkData&& Other) noexcept : State(std::move(Other.State))
	{
		Other.State = NewEmptyState();
	}

	auto FBulkData::operator=(FBulkData&& Other) noexcept -> FBulkData&
	{
		if (this != &Other)
		{
			State = std::move(Other.State);
			Other.State = NewEmptyState();
		}
		return *this;
	}

	auto FBulkData::TryCreateDetached(
		FByteView Bytes, FBulkData& OutValue, std::string* OutError) -> bool
	{
		if (Bytes.size() > MaximumBulkDataBytes)
			return BulkDataFail("Detached bulk data exceeds the 1 GiB limit.", OutError);
		auto Candidate = NewEmptyState();
		Candidate->Metadata.LogicalSize = Bytes.size();
		Candidate->Metadata.Range.StoredSize = Bytes.size();
		Candidate->Allocation = std::make_shared<FByteBuffer>(Bytes.begin(), Bytes.end());
		Candidate->State = EBulkDataState::Detached;
		OutValue = FBulkData(std::move(Candidate));
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::TryAttach(
		FBulkDataMetadata Metadata, FBulkData& OutValue, std::string* OutError) -> bool
	{
		if (!ValidateMetadata(Metadata, OutError)) return false;
		auto Candidate = NewEmptyState();
		Candidate->Metadata = std::move(Metadata);
		Candidate->State = Candidate->Metadata.Range.Resource->IsRetired()
			? EBulkDataState::Retired : EBulkDataState::Attached;
		OutValue = FBulkData(std::move(Candidate));
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::GetState() const -> EBulkDataState
	{
		std::lock_guard Lock(State->Mutex);
		return State->State;
	}

	auto FBulkData::GetMetadata() const -> FBulkDataMetadata
	{
		std::lock_guard Lock(State->Mutex);
		return State->Metadata;
	}

	auto FBulkData::HasData() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return State->Metadata.LogicalSize != 0;
	}

	auto FBulkData::LockReadOnly(
		FByteView& OutBytes, std::string* OutError) -> bool
	{
		std::unique_lock Lock(State->Mutex);
		if (State->State == EBulkDataState::Attached || State->State == EBulkDataState::Failed)
		{
			const FBulkDataMetadata Metadata = State->Metadata;
			State->State = EBulkDataState::Loading;
			Lock.unlock();
			FPackageResourceReadResult Result = Metadata.Range.Resource->ReadRange(
				Metadata.Range.SegmentOffset, Metadata.Range.StoredSize);
			Lock.lock();
			if (!Result || Result.Buffer.GetSize() != Metadata.LogicalSize)
			{
				State->State = Result.Status == EPackageResourceReadStatus::Retired
					? EBulkDataState::Retired : EBulkDataState::Failed;
				return BulkDataFail(Result.Message.empty() ? "Bulk data range load failed." : Result.Message, OutError);
			}
			State->Allocation = std::make_shared<FByteBuffer>(
				Result.Buffer.GetBytes().begin(), Result.Buffer.GetBytes().end());
			State->State = EBulkDataState::Resident;
		}
		if (State->State != EBulkDataState::Resident
			&& State->State != EBulkDataState::ReadLocked
			&& State->State != EBulkDataState::Detached)
			return BulkDataFail("Bulk data cannot acquire a read lock in its current state.", OutError);
		++State->ReadLocks;
		State->State = EBulkDataState::ReadLocked;
		OutBytes = *State->Allocation;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::UnlockReadOnly(std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::ReadLocked || State->ReadLocks == 0)
			return BulkDataFail("Bulk data has no matching read lock.", OutError);
		if (--State->ReadLocks == 0)
			State->State = State->Metadata.Range.Resource
				? EBulkDataState::Resident : EBulkDataState::Detached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::LockReadWrite(
		FMutableByteView& OutBytes, std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::Detached && State->State != EBulkDataState::Empty)
			return BulkDataFail("Bulk data write locks require detached storage.", OutError);
		if (!State->Allocation) State->Allocation = std::make_shared<FByteBuffer>();
		else if (State->Allocation.use_count() != 1)
			State->Allocation = std::make_shared<FByteBuffer>(*State->Allocation);
		State->Metadata.Range.Resource.reset();
		State->State = EBulkDataState::WriteLocked;
		OutBytes = *State->Allocation;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::Resize(
		uint64 Size, FMutableByteView& OutBytes, std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::WriteLocked)
			return BulkDataFail("Bulk data resize requires a write lock.", OutError);
		if (Size > MaximumBulkDataBytes || Size > std::numeric_limits<size_t>::max())
			return BulkDataFail("Bulk data resize exceeds the 1 GiB limit.", OutError);
		State->Allocation->resize(static_cast<size_t>(Size));
		State->Metadata.LogicalSize = Size;
		State->Metadata.Range.StoredSize = Size;
		OutBytes = *State->Allocation;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::UnlockWrite(std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::WriteLocked)
			return BulkDataFail("Bulk data has no matching write lock.", OutError);
		State->State = EBulkDataState::Detached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::Unload(std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::Resident || !State->Metadata.Range.Resource)
			return BulkDataFail("Only unlocked resource-backed bulk data can unload.", OutError);
		State->Allocation.reset();
		State->State = State->Metadata.Range.Resource->IsRetired()
			? EBulkDataState::Retired : EBulkDataState::Attached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::ReloadAsync() -> FPackageResourceRequest
	{
		FBulkDataMetadata Metadata;
		{
			std::lock_guard Lock(State->Mutex);
			if ((State->State == EBulkDataState::Resident
					|| State->State == EBulkDataState::Detached)
				&& State->Allocation)
			{
				return FPackageResourceRequest::Completed({
					.Status = EPackageResourceReadStatus::Success,
					.Buffer = FSharedByteBuffer::Share(State->Allocation)});
			}
			if ((State->State != EBulkDataState::Attached && State->State != EBulkDataState::Failed)
				|| !State->Metadata.Range.Resource)
				return FPackageResourceRequest::Completed({
					.Status = EPackageResourceReadStatus::InvalidRange,
					.Message = "Bulk data cannot reload in its current state."});
			Metadata = State->Metadata;
			State->State = EBulkDataState::Loading;
		}
		FPackageResourceRequest Request = Metadata.Range.Resource->ReadRangeAsync(
			Metadata.Range.SegmentOffset, Metadata.Range.StoredSize);
		auto Target = State;
		return FPackageResourceRequest::Transform(std::move(Request),
			[Target, Metadata](FPackageResourceReadResult Result) {
			std::lock_guard Lock(Target->Mutex);
			if (Target->State != EBulkDataState::Loading) return Result;
			if (Result && Result.Buffer.GetSize() == Metadata.LogicalSize)
			{
				Target->Allocation = std::make_shared<FByteBuffer>(
					Result.Buffer.GetBytes().begin(), Result.Buffer.GetBytes().end());
				Target->State = EBulkDataState::Resident;
			}
			else Target->State = Result.Status == EPackageResourceReadStatus::Retired
				? EBulkDataState::Retired : EBulkDataState::Failed;
			return Result;
		});
	}

	auto FBulkData::Serialize(
		FArchive& Ar, FArchiveBulkDataParameters Parameters) -> void
	{
		FArchiveBulkDataValue Value;
		if (Ar.IsSaving())
		{
			FBulkDataMetadata Metadata;
			std::shared_ptr<FByteBuffer> Allocation;
			{
				std::lock_guard Lock(State->Mutex);
				Metadata = State->Metadata;
				Allocation = State->Allocation;
			}
			FSharedByteBuffer Buffer;
			if (Allocation) Buffer = FSharedByteBuffer::Copy(*Allocation);
			else if (Metadata.Range.Resource)
			{
				const FPackageResourceReadResult Result = Metadata.Range.Resource->ReadRange(
					Metadata.Range.SegmentOffset, Metadata.Range.StoredSize);
				if (!Result)
				{
					Ar.Fail(EArchiveFailureCode::InvalidData,
						Result.Message.empty() ? "Bulk data cannot be read for serialization."
							: Result.Message);
					return;
				}
				Buffer = Result.Buffer;
			}
			if (Buffer.GetSize() != Metadata.LogicalSize)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Bulk data serialization captured an inconsistent logical size.");
				return;
			}
			Value.LogicalSize = Metadata.LogicalSize;
			Value.StoredSize = Metadata.LogicalSize;
			Value.ContentHash = FXxHash128::HashBuffer(Buffer.GetBytes());
			Value.Buffer = std::move(Buffer);
		}
		if (!Parameters.Owner) Parameters.Owner = this;
		Ar.SerializeBulkData(Value, Parameters);
		if (!Ar.IsLoading() || Ar.HasError()) return;

		FBulkData Candidate;
		std::string Error;
		const bool bLoaded = Value.StorageKind == EArchiveBulkDataStorageKind::External
			? Value.PackageResource && TryAttach({
				.LogicalSize = Value.LogicalSize,
				.Range = {
					.Resource = std::static_pointer_cast<FPackageResource>(Value.PackageResource),
					.SegmentOffset = Value.SegmentOffset,
					.StoredSize = Value.StoredSize,
					.Alignment = Value.Alignment}}, Candidate, &Error)
			: Value.Buffer.GetSize() == Value.LogicalSize
				&& TryCreateDetached(Value.Buffer.GetBytes(), Candidate, &Error);
		if (!bLoaded)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				Error.empty() ? "Loaded runtime bulk data is invalid." : Error);
			return;
		}
		*this = std::move(Candidate);
	}
}
