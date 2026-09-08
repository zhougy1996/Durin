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
				|| Metadata.LogicalSize > MaximumBulkDataBytes)
				return BulkDataFail("Bulk data package metadata is invalid or unsupported.", OutError);
			if (!ValidatePackageResourceRange(Metadata.Range, MaximumBulkDataBytes, OutError)) return false;
			if (OutError) OutError->clear();
			return true;
		}

		auto Snapshot(const std::shared_ptr<AssetPrivate::FBulkDataState>& Source)
			-> std::shared_ptr<AssetPrivate::FBulkDataState>
		{
			auto Result = NewEmptyState();
			std::lock_guard Lock(Source->Mutex);
			require(Source->State != EBulkDataState::WriteLocked);
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

	FBulkDataReadScope::~FBulkDataReadScope() { Reset(); }
	FBulkDataReadScope::FBulkDataReadScope(FBulkDataReadScope&& Other) noexcept
		: State(std::move(Other.State))
	{
	}
	auto FBulkDataReadScope::operator=(FBulkDataReadScope&& Other) noexcept -> FBulkDataReadScope&
	{
		if (this != &Other)
		{
			Reset();
			State = std::move(Other.State);
		}
		return *this;
	}
	auto FBulkDataReadScope::GetBytes() const -> FByteView
	{
		require(State != nullptr);
		std::lock_guard Guard(State->Mutex);
		check(State->State == EBulkDataState::ReadLocked);
		return *State->Allocation;
	}
	auto FBulkDataReadScope::Reset() -> void
	{
		if (!State) return;
		auto Previous = std::move(State);
		std::lock_guard Guard(Previous->Mutex);
		// Only the lease implementation can change the matching read count.
		check(Previous->State == EBulkDataState::ReadLocked && Previous->ReadLocks > 0);
		if (--Previous->ReadLocks == 0)
			Previous->State = Previous->Metadata.Range.Resource ? EBulkDataState::Resident : EBulkDataState::Detached;
	}

	FBulkDataWriteScope::~FBulkDataWriteScope() { Reset(); }
	FBulkDataWriteScope::FBulkDataWriteScope(FBulkDataWriteScope&& Other) noexcept
		: State(std::move(Other.State))
	{
	}
	auto FBulkDataWriteScope::operator=(FBulkDataWriteScope&& Other) noexcept -> FBulkDataWriteScope&
	{
		if (this != &Other)
		{
			Reset();
			State = std::move(Other.State);
		}
		return *this;
	}
	auto FBulkDataWriteScope::GetBytes() const -> FMutableByteView
	{
		require(State != nullptr);
		std::lock_guard Guard(State->Mutex);
		check(State->State == EBulkDataState::WriteLocked);
		return *State->Allocation;
	}
	auto FBulkDataWriteScope::TryResize(uint64 Size) -> bool
	{
		require(State != nullptr);
		std::lock_guard Guard(State->Mutex);
		check(State->State == EBulkDataState::WriteLocked);
		if (Size > MaximumBulkDataBytes || Size > std::numeric_limits<size_t>::max()) return false;
		State->Allocation->resize(static_cast<size_t>(Size));
		State->Metadata.LogicalSize = Size;
		State->Metadata.Range.StoredSize = Size;
		return true;
	}
	auto FBulkDataWriteScope::Reset() -> void
	{
		if (!State) return;
		auto Previous = std::move(State);
		std::lock_guard Guard(Previous->Mutex);
		check(Previous->State == EBulkDataState::WriteLocked);
		Previous->State = EBulkDataState::Detached;
	}

	auto FBulkData::AcquireRead() -> FBulkDataReadResult
	{
		std::unique_lock Guard(State->Mutex);
		if (State->State == EBulkDataState::Attached || State->State == EBulkDataState::Failed)
		{
			const FBulkDataMetadata Metadata = State->Metadata;
			State->State = EBulkDataState::Loading;
			Guard.unlock();
			auto Result = Metadata.Range.Resource->ReadRange(Metadata.Range.SegmentOffset, Metadata.Range.StoredSize);
			Guard.lock();
			if (Result && Result.Buffer.GetSize() != Metadata.LogicalSize)
				Result = {.Status = EPackageResourceReadStatus::TruncatedSegment, .Message = "Bulk data logical size does not match the read."};
			if (!Result)
			{
				State->State = Result.Status == EPackageResourceReadStatus::Retired ? EBulkDataState::Retired : EBulkDataState::Failed;
				return {.Status = State->State == EBulkDataState::Retired ? EBulkReadStatus::Retired : EBulkReadStatus::ReadFailed, .Error = std::move(Result)};
			}
			State->Allocation = std::make_shared<FByteBuffer>(Result.Buffer.GetBytes().begin(), Result.Buffer.GetBytes().end());
			State->State = EBulkDataState::Resident;
		}
		if (State->State == EBulkDataState::Retired)
			return {.Status = EBulkReadStatus::Retired, .Error = {.Status = EPackageResourceReadStatus::Retired, .Message = "Bulk data resource is retired."}};
		if (State->State != EBulkDataState::Resident && State->State != EBulkDataState::ReadLocked && State->State != EBulkDataState::Detached)
			return {.Status = State->State == EBulkDataState::Empty ? EBulkReadStatus::Empty : EBulkReadStatus::Busy, .Error = {.Message = "Bulk data is empty, loading, or write locked."}};
		++State->ReadLocks;
		State->State = EBulkDataState::ReadLocked;
		return {.Status = EBulkReadStatus::Acquired, .Lock = FBulkDataReadScope(State)};
	}

	auto FBulkData::AcquireWrite() -> FBulkDataWriteScope
	{
		std::lock_guard Guard(State->Mutex);
		require(State->State == EBulkDataState::Detached || State->State == EBulkDataState::Empty);
		if (!State->Allocation) State->Allocation = std::make_shared<FByteBuffer>();
		else if (State->Allocation.use_count() != 1)
			State->Allocation = std::make_shared<FByteBuffer>(*State->Allocation);
		State->Metadata.Range.Resource.reset();
		State->State = EBulkDataState::WriteLocked;
		return FBulkDataWriteScope(State);
	}

	auto FBulkData::TryUnload() -> EBulkUnloadResult
	{
		std::lock_guard Guard(State->Mutex);
		if (State->State == EBulkDataState::ReadLocked || State->State == EBulkDataState::WriteLocked || State->State == EBulkDataState::Loading)
			return EBulkUnloadResult::Busy;
		if (State->State != EBulkDataState::Resident || !State->Metadata.Range.Resource) return EBulkUnloadResult::NotResident;
		State->Allocation.reset();
		State->State = State->Metadata.Range.Resource->IsRetired() ? EBulkDataState::Retired : EBulkDataState::Attached;
		return EBulkUnloadResult::Unloaded;
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
