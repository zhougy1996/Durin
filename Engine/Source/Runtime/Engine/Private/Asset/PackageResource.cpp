#include "Asset/PackageResource.h"

#include "Misc/FileHelper.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace AssetPrivate
	{
		struct FPackageResourceRequestState
		{
			explicit FPackageResourceRequestState(bool bInAwaitingTask = false)
				: bAwaitingTask(bInAwaitingTask) {}

			mutable std::mutex Mutex;
			std::condition_variable Ready;
			std::atomic_bool bCancelled = false;
			bool bAwaitingTask = false;
			bool bTerminal = false;
			FPackageResourceReadResult Result;
			std::function<void()> OnCancel;
			FTaskHandle Task;

			auto Complete(FPackageResourceReadResult InResult) -> void
			{
				{
					std::lock_guard Lock(Mutex);
					if (bTerminal) return;
					Result = std::move(InResult);
					bTerminal = true;
				}
				Ready.notify_all();
			}

			auto SetTask(FTaskHandle InTask) -> void
			{
				bool bCancelTask = false;
				{
					std::lock_guard Lock(Mutex);
					Task = std::move(InTask);
					bAwaitingTask = false;
					bCancelTask = bCancelled.load(std::memory_order_acquire);
				}
				Ready.notify_all();
				if (bCancelTask && Task.IsValid()) CancelTask(Task);
			}
		};
	}

	namespace
	{
		constexpr size_t PackageValidationScratchBytes = 64 * 1024;

		auto Result(EPackageResourceReadStatus Status, std::string Message = {})
			-> FPackageResourceReadResult
		{
			return {.Status = Status, .Message = std::move(Message)};
		}

		auto ValidateLoosePackageGeneration(
			const std::filesystem::path& Path,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries,
			FPackageResourceReadStats& Stats,
			std::string& OutError) -> bool
		{
			FFileHelper::FFileIoError FileError;
			auto File = FFileHelper::OpenRead(Path, &FileError);
			if (!File)
			{
				OutError = FileError.ToString();
				return false;
			}
			if (File->GetSize() != Summary.Extent)
			{
				OutError = "Loose package bulk segment extent does not match the package generation.";
				return false;
			}

			std::array<std::byte, PackageValidationScratchBytes> Scratch{};
			Stats.PeakValidationScratchBytes = std::max<uint64>(
				Stats.PeakValidationScratchBytes, Scratch.size());
			FXxHash128Builder SegmentHash;
			FXxHash128Builder FieldHash;
			size_t EntryIndex = 0;
			auto AdvanceToExternal = [&] {
				while (EntryIndex < Entries.size()
					&& Entries[EntryIndex].Placement != EPackageBulkDataPlacement::External)
					++EntryIndex;
			};
			AdvanceToExternal();
			for (uint64 Offset = 0; Offset < Summary.Extent;)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64>(
					Scratch.size(), Summary.Extent - Offset));
				auto Bytes = std::span(Scratch).first(Count);
				if (!File->ReadAt(Offset, Bytes, &FileError))
				{
					OutError = FileError.ToString();
					return false;
				}
				++Stats.ValidationReadCount;
				Stats.ValidationBytesRead += Count;
				SegmentHash.Update(Bytes);

				size_t LocalOffset = 0;
				while (LocalOffset < Count && EntryIndex < Entries.size())
				{
					const FPackageBulkDataEntry& Entry = Entries[EntryIndex];
					const uint64 AbsoluteOffset = Offset + LocalOffset;
					if (AbsoluteOffset < Entry.SegmentOffset)
					{
						const size_t PaddingBytes = static_cast<size_t>(std::min<uint64>(
							Count - LocalOffset, Entry.SegmentOffset - AbsoluteOffset));
						if (std::ranges::any_of(Bytes.subspan(LocalOffset, PaddingBytes),
								[](std::byte Byte) { return Byte != std::byte{0}; }))
						{
							OutError = "Loose package bulk segment has nonzero alignment padding.";
							return false;
						}
						LocalOffset += PaddingBytes;
						continue;
					}

					const uint64 EntryEnd = Entry.SegmentOffset + Entry.StoredSize;
					const size_t PayloadBytes = static_cast<size_t>(std::min<uint64>(
						Count - LocalOffset, EntryEnd - AbsoluteOffset));
					FieldHash.Update(Bytes.subspan(LocalOffset, PayloadBytes));
					LocalOffset += PayloadBytes;
					if (AbsoluteOffset + PayloadBytes == EntryEnd)
					{
						if (FieldHash.Finalize() != Entry.ContentId)
						{
							OutError = "Loose package bulk field digest does not match its directory entry.";
							return false;
						}
						FieldHash.Reset();
						++EntryIndex;
						AdvanceToExternal();
					}
				}
				Offset += Count;
			}
			if (EntryIndex != Entries.size() || SegmentHash.Finalize() != Summary.Digest)
			{
				OutError = "Loose package bulk segment digest does not match the package generation.";
				return false;
			}
			OutError.clear();
			return true;
		}

		class FLoosePackageResource final : public FPackageResource
		{
		public:
			FLoosePackageResource(std::filesystem::path InSegmentPath, uint64 Extent,
				FPackageResourceReadStats ValidationStats)
				: FPackageResource(Extent, ValidationStats),
				  SegmentPath(std::move(InSegmentPath)) {}

		private:
			auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool& bCancelled)
				-> FPackageResourceReadResult override
			{
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				std::error_code Error;
				const uint64 BeforeSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error)
					return Result(Error == std::errc::no_such_file_or_directory
						? EPackageResourceReadStatus::MissingSegment
						: EPackageResourceReadStatus::IoError,
						"Package bulk segment cannot be opened.");
				if (BeforeSize != GetSegmentExtent())
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment extent changed after registration.");

				std::ifstream Stream(SegmentPath, std::ios::binary);
				if (!Stream.is_open())
					return Result(EPackageResourceReadStatus::IoError,
						"Package bulk segment range cannot be opened.");
				Stream.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
				if (!Stream)
					return Result(EPackageResourceReadStatus::IoError,
						"Package bulk segment range seek failed.");
				FByteBuffer Bytes(static_cast<size_t>(Size));
				if (Size != 0)
					Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size));
				if (Stream.gcount() != static_cast<std::streamsize>(Size))
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment range is truncated.");
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				const uint64 AfterSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error || AfterSize != BeforeSize)
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment changed during a range read.");
				return {.Status = EPackageResourceReadStatus::Success,
					.Buffer = FSharedByteBuffer::Take(std::move(Bytes))};
			}

			std::filesystem::path SegmentPath;
		};

		auto CompleteRetired() -> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::Retired, "Package resource is retired."));
		}

		auto PackageResourceAttribution() -> FTaskAttribution
		{
			static const FTaskAttribution Attribution =
				RegisterTaskAttribution("Engine", "PackageResource");
			return Attribution;
		}
	}

	auto FPackageResourceRequest::IsReady() const -> bool
	{
		if (!State) return true;
		std::lock_guard Lock(State->Mutex);
		return State->bTerminal || (!State->bAwaitingTask && State->Task.IsComplete());
	}

	auto FPackageResourceRequest::Cancel() -> void
	{
		if (!State) return;
		State->bCancelled.store(true, std::memory_order_release);
		std::function<void()> OnCancel;
		FTaskHandle Task;
		{
			std::lock_guard Lock(State->Mutex);
			OnCancel = State->OnCancel;
			Task = State->Task;
		}
		if (OnCancel) OnCancel();
		if (Task.IsValid()) CancelTask(Task);
	}

	auto FPackageResourceRequest::Wait() const -> FPackageResourceReadResult
	{
		if (!State) return Result(EPackageResourceReadStatus::Retired, "Package request is invalid.");
		FTaskHandle Task;
		{
			std::unique_lock Lock(State->Mutex);
			State->Ready.wait(Lock, [&] {
				return State->bTerminal || !State->bAwaitingTask;
			});
			if (State->bTerminal) return State->Result;
			Task = State->Task;
		}
		const FTaskWaitResult WaitResult = WaitTask(Task);
		{
			std::lock_guard Lock(State->Mutex);
			if (State->bTerminal) return State->Result;
		}
		if (WaitResult.WaitStatus != ETaskWaitStatus::Completed)
			return Result(EPackageResourceReadStatus::IoError,
				"Package request wait was rejected; the request outcome is unchanged.");
		const ETaskState TaskState = WaitResult.TaskState;
		if (TaskState == ETaskState::Canceled)
			State->Complete(Result(EPackageResourceReadStatus::Cancelled,
				"Package request task was cancelled."));
		else
			State->Complete(Result(EPackageResourceReadStatus::IoError,
				Task.IsValid() ? Task.GetDiagnostic()
					: "Package request task admission was rejected."));
		std::lock_guard Lock(State->Mutex);
		return State->Result;
	}

	auto FPackageResourceRequest::Completed(FPackageResourceReadResult InResult)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>();
		State->Complete(std::move(InResult));
		return FPackageResourceRequest(std::move(State));
	}

	auto FPackageResourceRequest::Transform(
		FPackageResourceRequest Input,
		std::function<FPackageResourceReadResult(FPackageResourceReadResult)> Function)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>(true);
		FTaskHandle InputTask;
		if (Input.State)
		{
			std::unique_lock Lock(Input.State->Mutex);
			Input.State->Ready.wait(Lock, [&Input] {
				return Input.State->bTerminal || !Input.State->bAwaitingTask;
			});
			InputTask = Input.State->Task;
		}
		State->OnCancel = [Input]() mutable { Input.Cancel(); };
		auto TransformFunction = [State, Input = std::move(Input),
			Function = std::move(Function)]() mutable {
			State->Complete(Function(Input.Wait()));
		};
		// A direct completed request has no task edge, so schedule the transform as a root.
		// Otherwise the completion edge guarantees Input.Wait() cannot occupy a Worker.
		FTaskHandle Task = InputTask.IsValid()
			? ThenOutcome(InputTask, "PackageResource.Transform",
				[Function = std::move(TransformFunction)](FTaskOutcome<void>) mutable {
					Function();
				}, {.Attribution = PackageResourceAttribution()})
			: LaunchTask("PackageResource.Transform", std::move(TransformFunction),
				{.Attribution = PackageResourceAttribution()});
		if (!Task.IsValid())
			State->Complete(Result(EPackageResourceReadStatus::IoError,
				"Package transform task admission was rejected."));
		State->SetTask(std::move(Task));
		return FPackageResourceRequest(std::move(State));
	}

	FPackageResource::~FPackageResource()
	{
		Retire();
	}

	auto FPackageResource::ReadRangeAsync(uint64 Offset, uint64 Size)
		-> FPackageResourceRequest
	{
		if (Offset > SegmentExtent || Size > SegmentExtent - Offset
			|| Size > static_cast<uint64>(std::numeric_limits<size_t>::max()))
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::InvalidRange, "Package resource range is invalid."));

		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>(true);
		{
			std::lock_guard Lock(Mutex);
			if (bRetired) return CompleteRetired();
			++ReadStats.RequestCount;
			ReadStats.RequestedBytes = Size
				> std::numeric_limits<uint64>::max() - ReadStats.RequestedBytes
				? std::numeric_limits<uint64>::max()
				: ReadStats.RequestedBytes + Size;
			Requests.push_back(State);
		}
		auto Self = shared_from_this();
		FTaskHandle Task = LaunchCancelableTask("PackageResource.ReadRange",
			[Self = std::move(Self), State, Offset, Size](const FTaskCancellationToken& Token) {
			FPackageResourceReadResult ReadResult;
			if (Token.IsCancellationRequested()
				|| State->bCancelled.load(std::memory_order_acquire))
				ReadResult = Result(EPackageResourceReadStatus::Cancelled,
					"Package range request was cancelled.");
			else
				ReadResult = Self->ReadRangeImpl(Offset, Size, State->bCancelled);
			State->Complete(std::move(ReadResult));
		}, {.Attribution = PackageResourceAttribution()});
		if (!Task.IsValid())
			State->Complete(Result(EPackageResourceReadStatus::IoError,
				"Package read task admission was rejected."));
		State->SetTask(std::move(Task));
		return FPackageResourceRequest(std::move(State));
	}

	auto FPackageResource::ReadRange(uint64 Offset, uint64 Size)
		-> FPackageResourceReadResult
	{
		return ReadRangeAsync(Offset, Size).Wait();
	}

	auto FPackageResource::Retire() -> void
	{
		std::vector<std::shared_ptr<AssetPrivate::FPackageResourceRequestState>> Active;
		{
			std::lock_guard Lock(Mutex);
			bRetired = true;
			for (auto& Weak : Requests)
				if (auto State = Weak.lock()) Active.push_back(std::move(State));
		}
		for (const auto& State : Active)
		{
			FPackageResourceRequest Request(State);
			Request.Cancel();
		}
		for (const auto& State : Active)
			(void)FPackageResourceRequest(State).Wait();
		std::lock_guard Lock(Mutex);
		Requests.clear();
	}

	auto FPackageResource::IsRetired() const -> bool
	{
		std::lock_guard Lock(Mutex);
		return bRetired;
	}

	auto FPackageResource::GetReadStats() const -> FPackageResourceReadStats
	{
		std::lock_guard Lock(Mutex);
		return ReadStats;
	}

	auto ValidatePackageResourceRange(
		const FPackageResourceRange& Range,
		uint64 MaximumStoredSize,
		std::string* OutError) -> bool
	{
		const bool bValid = Range.Resource && Range.StorageFlags == 0
			&& Range.StoredSize <= MaximumStoredSize
			&& Range.Alignment != 0 && Range.Alignment <= 4096
			&& (Range.Alignment & (Range.Alignment - 1)) == 0
			&& Range.SegmentOffset % Range.Alignment == 0
			&& Range.SegmentOffset <= Range.Resource->GetSegmentExtent()
			&& Range.StoredSize <= Range.Resource->GetSegmentExtent() - Range.SegmentOffset;
		if (!bValid)
		{
			if (OutError) *OutError = "Package resource range is invalid or unsupported.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
	}

	FPackageResourceManager::~FPackageResourceManager()
	{
		Shutdown();
	}

	auto FPackageResourceManager::RegisterLoosePackage(
		std::string LogicalPackageId,
		const std::filesystem::path& PackagePath,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FPackageResourceHandle& OutHandle,
		std::string* OutError) -> bool
	{
		if (LogicalPackageId.empty() || Summary.Extent == 0
			|| !ValidatePackageBulkDataMetadata(Summary, Entries, OutError)) return false;
		std::filesystem::path SegmentPath = PackagePath;
		SegmentPath.replace_extension(".dbulk");
		std::filesystem::path BackupPath = SegmentPath;
		BackupPath += ".durin-backup";
		std::error_code Error;
		FPackageResourceReadStats ValidationStats;
		std::string PrimaryValidationError;
		if (!ValidateLoosePackageGeneration(
				SegmentPath, Summary, Entries, ValidationStats, PrimaryValidationError))
		{
			std::string BackupValidationError;
			if (!ValidateLoosePackageGeneration(
					BackupPath, Summary, Entries, ValidationStats, BackupValidationError))
			{
				if (OutError) *OutError =
					"Loose package bulk segment does not match the package generation: "
					+ PrimaryValidationError + " Backup validation failed: "
					+ BackupValidationError;
				return false;
			}
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::CopyFileAtomically(
					BackupPath, SegmentPath, &PublicationError))
			{
				if (OutError) *OutError = "Loose package bulk backup recovery failed.";
				return false;
			}
			std::string RecoveredValidationError;
			if (!ValidateLoosePackageGeneration(SegmentPath, Summary, Entries,
					ValidationStats, RecoveredValidationError))
			{
				if (OutError) *OutError =
					"Recovered loose package bulk segment failed validation: "
					+ RecoveredValidationError;
				return false;
			}
		}
		std::filesystem::remove(BackupPath, Error);

		auto Resource = std::make_shared<FLoosePackageResource>(
			SegmentPath, Summary.Extent, ValidationStats);
		FPackageResourceHandle Previous;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown)
			{
				if (OutError) *OutError = "Package resource manager is shut down.";
				return false;
			}
			auto& Slot = Resources[std::move(LogicalPackageId)];
			Previous = std::move(Slot);
			Slot = Resource;
		}
		if (Previous) Previous->Retire();
		OutHandle = std::move(Resource);
		if (OutError) OutError->clear();
		return true;
	}

	auto FPackageResourceManager::RetirePackage(std::string_view LogicalPackageId) -> void
	{
		FPackageResourceHandle Resource;
		{
			std::lock_guard Lock(Mutex);
			const auto It = Resources.find(std::string(LogicalPackageId));
			if (It == Resources.end()) return;
			Resource = std::move(It->second);
			Resources.erase(It);
		}
		Resource->Retire();
	}

	auto FPackageResourceManager::FindPackage(std::string_view LogicalPackageId) const
		-> FPackageResourceHandle
	{
		std::lock_guard Lock(Mutex);
		const auto It = Resources.find(std::string(LogicalPackageId));
		return It == Resources.end() ? FPackageResourceHandle{} : It->second;
	}

	auto FPackageResourceManager::GetRegisteredPackageCount() const -> uint64
	{
		std::lock_guard Lock(Mutex);
		return static_cast<uint64>(Resources.size());
	}

	auto FPackageResourceManager::RetireAllPackages() -> void
	{
		std::unordered_map<std::string, FPackageResourceHandle> Retiring;
		{
			std::lock_guard Lock(Mutex);
			Retiring = std::move(Resources);
		}
		for (auto& [Name, Resource] : Retiring) Resource->Retire();
	}

	auto FPackageResourceManager::Shutdown() -> void
	{
		std::unordered_map<std::string, FPackageResourceHandle> Retiring;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown) return;
			bShutdown = true;
			Retiring = std::move(Resources);
		}
		for (auto& [Name, Resource] : Retiring) Resource->Retire();
	}

	auto GetPackageResourceManager() -> FPackageResourceManager&
	{
		static FPackageResourceManager Manager;
		return Manager;
	}
}
