#include "Asset/BulkData.h"

namespace Durin::Asset
{
	namespace Private
	{
		struct FBulkDataState
		{
			mutable std::mutex Mutex;
			FBulkDataMetadata Metadata;
			std::shared_ptr<std::vector<std::byte>> Allocation;
			EBulkDataState State = EBulkDataState::Empty;
			uint32 ReadLocks = 0;
		};
	}

	namespace
	{
		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto NewEmptyState() -> std::shared_ptr<Private::FBulkDataState>
		{
			return std::make_shared<Private::FBulkDataState>();
		}

		auto ValidateMetadata(const FBulkDataMetadata& Metadata, std::string* OutError) -> bool
		{
			if (!Metadata.Resource || Metadata.StorageFlags != 0
				|| Metadata.LogicalSize != Metadata.StoredSize
				|| Metadata.LogicalSize > MaximumBulkDataBytes
				|| Metadata.Alignment == 0 || Metadata.Alignment > 4096
				|| (Metadata.Alignment & (Metadata.Alignment - 1)) != 0
				|| Metadata.SegmentOffset % Metadata.Alignment != 0
				|| Metadata.SegmentOffset > Metadata.Resource->GetSegmentExtent()
				|| Metadata.StoredSize > Metadata.Resource->GetSegmentExtent() - Metadata.SegmentOffset)
				return Fail("Bulk data package metadata is invalid or unsupported.", OutError);
			if (OutError) OutError->clear();
			return true;
		}

		auto Snapshot(const std::shared_ptr<Private::FBulkDataState>& Source)
			-> std::shared_ptr<Private::FBulkDataState>
		{
			auto Result = NewEmptyState();
			std::lock_guard Lock(Source->Mutex);
			Result->Metadata = Source->Metadata;
			Result->Allocation = Source->Allocation;
			if (Source->State == EBulkDataState::Empty) Result->State = EBulkDataState::Empty;
			else if (Source->State == EBulkDataState::Detached || !Source->Metadata.Resource)
				Result->State = EBulkDataState::Detached;
			else if (Source->Allocation) Result->State = EBulkDataState::Resident;
			else if (Source->Metadata.Resource->IsRetired()) Result->State = EBulkDataState::Retired;
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
		std::span<const std::byte> Bytes, FBulkData& OutValue, std::string* OutError) -> bool
	{
		if (Bytes.size() > MaximumBulkDataBytes)
			return Fail("Detached bulk data exceeds the 1 GiB limit.", OutError);
		auto Candidate = NewEmptyState();
		Candidate->Metadata.LogicalSize = Bytes.size();
		Candidate->Metadata.StoredSize = Bytes.size();
		Candidate->Allocation = std::make_shared<std::vector<std::byte>>(Bytes.begin(), Bytes.end());
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
		Candidate->State = Candidate->Metadata.Resource->IsRetired()
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
		std::span<const std::byte>& OutBytes, std::string* OutError) -> bool
	{
		std::unique_lock Lock(State->Mutex);
		if (State->State == EBulkDataState::Attached || State->State == EBulkDataState::Failed)
		{
			const FBulkDataMetadata Metadata = State->Metadata;
			State->State = EBulkDataState::Loading;
			Lock.unlock();
			FPackageResourceReadResult Result = Metadata.Resource->ReadRange(
				Metadata.SegmentOffset, Metadata.StoredSize);
			Lock.lock();
			if (!Result || Result.Buffer.GetSize() != Metadata.LogicalSize)
			{
				State->State = Result.Status == EPackageResourceReadStatus::Retired
					? EBulkDataState::Retired : EBulkDataState::Failed;
				return Fail(Result.Message.empty() ? "Bulk data range load failed." : Result.Message, OutError);
			}
			State->Allocation = std::make_shared<std::vector<std::byte>>(
				Result.Buffer.GetBytes().begin(), Result.Buffer.GetBytes().end());
			State->State = EBulkDataState::Resident;
		}
		if (State->State != EBulkDataState::Resident
			&& State->State != EBulkDataState::ReadLocked
			&& State->State != EBulkDataState::Detached)
			return Fail("Bulk data cannot acquire a read lock in its current state.", OutError);
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
			return Fail("Bulk data has no matching read lock.", OutError);
		if (--State->ReadLocks == 0)
			State->State = State->Metadata.Resource ? EBulkDataState::Resident : EBulkDataState::Detached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::LockReadWrite(
		std::span<std::byte>& OutBytes, std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::Detached && State->State != EBulkDataState::Empty)
			return Fail("Bulk data write locks require detached storage.", OutError);
		if (!State->Allocation) State->Allocation = std::make_shared<std::vector<std::byte>>();
		else if (State->Allocation.use_count() != 1)
			State->Allocation = std::make_shared<std::vector<std::byte>>(*State->Allocation);
		State->Metadata.Resource.reset();
		State->State = EBulkDataState::WriteLocked;
		OutBytes = *State->Allocation;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::Resize(
		uint64 Size, std::span<std::byte>& OutBytes, std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::WriteLocked)
			return Fail("Bulk data resize requires a write lock.", OutError);
		if (Size > MaximumBulkDataBytes || Size > std::numeric_limits<size_t>::max())
			return Fail("Bulk data resize exceeds the 1 GiB limit.", OutError);
		State->Allocation->resize(static_cast<size_t>(Size));
		State->Metadata.LogicalSize = Size;
		State->Metadata.StoredSize = Size;
		OutBytes = *State->Allocation;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::UnlockWrite(std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::WriteLocked)
			return Fail("Bulk data has no matching write lock.", OutError);
		State->State = EBulkDataState::Detached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::Unload(std::string* OutError) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		if (State->State != EBulkDataState::Resident || !State->Metadata.Resource)
			return Fail("Only unlocked resource-backed bulk data can unload.", OutError);
		State->Allocation.reset();
		State->State = State->Metadata.Resource->IsRetired()
			? EBulkDataState::Retired : EBulkDataState::Attached;
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::ReloadAsync() -> FPackageResourceRequest
	{
		FBulkDataMetadata Metadata;
		{
			std::lock_guard Lock(State->Mutex);
			if ((State->State != EBulkDataState::Attached && State->State != EBulkDataState::Failed)
				|| !State->Metadata.Resource)
				return FPackageResourceRequest::Completed({
					.Status = EPackageResourceReadStatus::InvalidRange,
					.Message = "Bulk data cannot reload in its current state."});
			Metadata = State->Metadata;
			State->State = EBulkDataState::Loading;
		}
		FPackageResourceRequest Request = Metadata.Resource->ReadRangeAsync(
			Metadata.SegmentOffset, Metadata.StoredSize);
		auto Target = State;
		return FPackageResourceRequest::Transform(std::move(Request),
			[Target, Metadata](FPackageResourceReadResult Result) {
			std::lock_guard Lock(Target->Mutex);
			if (Target->State != EBulkDataState::Loading) return Result;
			if (Result && Result.Buffer.GetSize() == Metadata.LogicalSize)
			{
				Target->Allocation = std::make_shared<std::vector<std::byte>>(
					Result.Buffer.GetBytes().begin(), Result.Buffer.GetBytes().end());
				Target->State = EBulkDataState::Resident;
			}
			else Target->State = Result.Status == EPackageResourceReadStatus::Retired
				? EBulkDataState::Retired : EBulkDataState::Failed;
			return Result;
		});
	}
}
