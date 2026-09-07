#include "Asset/PackageResource.h"

#include "AssetPackageCodec.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageInspection.h"
#include "Misc/FileHelper.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace AssetPrivate
	{
		// Keeps root admission open for copied requests and their later transforms.
		struct FPackageTaskLifetime
		{
			FTaskScope Scope = CreateTaskScope();
			~FPackageTaskLifetime() { Scope.Close(ETaskScopeCloseMode::Drain); }
		};

		struct FPackageResourceRequestState
		{
			using FSharedResult = Tasks::TSharedTask<FPackageResourceReadResult>;
			explicit FPackageResourceRequestState(bool bInBinding = false) : bBinding(bInBinding) {}
			// Synchronizes registration with resource retirement until the immutable
			// task is bound. Result publication and all later waits belong to Core.
			mutable std::mutex Mutex;
			std::condition_variable Bound;
			bool bBinding = false;
			std::variant<FPackageResourceReadResult, FSharedResult> Result;
			Tasks::FTaskCompletion RejectedProducer;
			std::atomic_bool bCancelled = false;
			std::function<void()> OnCancel;
			std::shared_ptr<FPackageTaskLifetime> Lifetime;
			uint64 EstimatedBytes = sizeof(FPackageResourceReadResult);

			auto Completion() const -> Tasks::FTaskCompletion
			{
				if (const auto* Task = std::get_if<FSharedResult>(&Result)) return Task->GetCompletion();
				return RejectedProducer;
			}
			auto Bind(Tasks::TTaskAdmission<Tasks::TTask<FPackageResourceReadResult>> Admission) -> void
			{
				std::variant<FPackageResourceReadResult, FSharedResult> Published;
				Tasks::FTaskCompletion Rejected;
				if (Admission.HasValue())
				{
					auto Task = std::move(Admission).TakeValue();
					Rejected = Task.GetCompletion();
					auto Shared = Tasks::Share(std::move(Task));
					if (Shared.HasValue()) { Published = std::move(Shared).TakeValue(); Rejected = {}; }
					else
					{
						Tasks::Cancel(Rejected);
						Published = FPackageResourceReadResult{.Status = EPackageResourceReadStatus::IoError,
							.Message = "Package result sharing was rejected."};
					}
				}
				else Published = FPackageResourceReadResult{.Status = EPackageResourceReadStatus::IoError,
					.Message = "Package task admission was rejected."};
				Tasks::FTaskCompletion CompletionToCancel;
				{
					std::lock_guard Lock(Mutex);
					Result = std::move(Published);
					RejectedProducer = std::move(Rejected);
					bBinding = false;
					if (bCancelled.load(std::memory_order_acquire)) CompletionToCancel = Completion();
				}
				Bound.notify_all();
				if (CompletionToCancel.IsValid()) Tasks::Cancel(CompletionToCancel);
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

		class FOwnedPackageResource final : public FPackageResource
		{
		public:
			explicit FOwnedPackageResource(FSharedByteBuffer InBytes)
				: FPackageResource(InBytes.GetSize()), Bytes(std::move(InBytes)) {}

		private:
			auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool& bCancelled)
				-> FPackageResourceReadResult override
			{
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				return {.Status = EPackageResourceReadStatus::Success,
					.Buffer = Bytes.MakeView(Offset, Size)};
			}

			FSharedByteBuffer Bytes;
		};

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

		class FSnapshotPackageResource final : public FPackageResource
		{
		public:
			explicit FSnapshotPackageResource(FSharedByteBuffer InBytes)
				: FPackageResource(InBytes.GetSize()), Bytes(std::move(InBytes)) {}

		private:
			auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool& bCancelled)
				-> FPackageResourceReadResult override
			{
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled);
				return {.Status = EPackageResourceReadStatus::Success,
					.Buffer = Bytes.MakeView(Offset, Size)};
			}
			FSharedByteBuffer Bytes;
		};

		using EPreparedStatus = EPreparedPackageResourceStatus;

		auto CheckSnapshotFile(const std::filesystem::path& Path, uint64 Extent,
			FXxHash128 Digest, const std::function<bool()>& IsCancelled)
			-> FPreparedPackageResourceResult
		{
			FFileHelper::FFileIoError Error;
			auto File = FFileHelper::OpenRead(Path, &Error);
			if (!File) return {EPreparedStatus::IoError, Error.ToString()};
			if (File->GetSize() != Extent)
				return {EPreparedStatus::Stale, "Package closure extent changed."};
			std::array<std::byte, PackageValidationScratchBytes> Scratch{};
			FXxHash128Builder Hash;
			for (uint64 Offset = 0; Offset < Extent;)
			{
				if (IsCancelled && IsCancelled())
					return {EPreparedStatus::Cancelled, "Package closure validation was cancelled."};
				auto Bytes = std::span(Scratch).first(static_cast<size_t>(
					std::min<uint64>(Scratch.size(), Extent - Offset)));
				if (!File->ReadAt(Offset, Bytes, &Error))
					return {EPreparedStatus::IoError, Error.ToString()};
				Hash.Update(Bytes);
				Offset += Bytes.size();
			}
			if (Hash.Finalize() != Digest)
				return {EPreparedStatus::Stale, "Package closure content changed."};
			return {};
		}

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

	auto FPreparedPackageResource::Read(const FPackagePath& LogicalPath,
		const std::filesystem::path& PackagePath, uint64 MaximumRetainedBytes,
		FPreparedPackageResource& Out, const std::function<bool()>& IsCancelled)
		-> FPreparedPackageResourceResult
	{
		if (IsCancelled && IsCancelled())
			return {EPreparedStatus::Cancelled, "Package closure read was cancelled."};
		if (!LogicalPath.IsValid())
			return {EPreparedStatus::InvalidClosure, "A valid logical package identity is required."};
		try
		{
			FFileHelper::FFileIoError FileError;
			auto File = FFileHelper::OpenRead(PackagePath, &FileError);
			if (!File) return {EPreparedStatus::IoError, FileError.ToString()};
			const uint64 MainSize = File->GetSize();
			if (MainSize > MaximumRetainedBytes || MainSize > std::numeric_limits<size_t>::max())
				return {EPreparedStatus::BudgetExceeded, "Package main file exceeds the retained byte budget."};
			FByteBuffer Main(static_cast<size_t>(MainSize));
			for (uint64 Offset = 0; Offset < MainSize;)
			{
				if (IsCancelled && IsCancelled())
					return {EPreparedStatus::Cancelled, "Package closure read was cancelled."};
				auto Chunk = std::span(Main).subspan(static_cast<size_t>(Offset),
					static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, MainSize - Offset)));
				if (!File->ReadAt(Offset, Chunk, &FileError))
					return {EPreparedStatus::IoError, FileError.ToString()};
				Offset += Chunk.size();
			}
			File.reset();
			auto BulkPath = PackagePath;
			BulkPath.replace_extension(".dbulk");
			std::error_code Error;
			uint64 BulkSize = 0;
			const bool bHasBulk = std::filesystem::exists(BulkPath, Error);
			if (!Error && bHasBulk) BulkSize = std::filesystem::file_size(BulkPath, Error);
			if (Error) return {EPreparedStatus::IoError, Error.message()};
			if (BulkSize > MaximumRetainedBytes - MainSize)
				return {EPreparedStatus::BudgetExceeded, "Package closure exceeds the retained byte budget."};
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (auto Result = AssetPrivate::ResolveAssetPackageReader(Main, Codec); !Result)
				return {EPreparedStatus::InvalidClosure, std::move(Result.Message)};
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Main, .PackagePath = LogicalPath,
				.PhysicalPackageBytes = MainSize, .PhysicalBulkBytes = BulkSize,
				.bResourceBackedBulk = true};
			FAssetPackageHeader Header;
			if (auto Result = Codec->ReadHeader(Context, Header); !Result)
				return {EPreparedStatus::InvalidClosure, std::move(Result.Message)};
			FAssetPackageInspection Inspection;
			if (auto Result = Codec->Inspect(Context, Inspection); !Result)
				return {EPreparedStatus::InvalidClosure, std::move(Result.Message)};
			std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
			std::string Diagnostic;
			if (!InspectEditorBulkDataStorageDescriptors(Inspection, Descriptors, &Diagnostic))
				return {EPreparedStatus::InvalidClosure, std::move(Diagnostic)};
			std::vector<FPackageBulkDataEntry> Entries;
			Entries.reserve(Descriptors.size());
			for (const auto& Descriptor : Descriptors)
				Entries.push_back({.FieldIndex = Entries.size() + 1,
					.Placement = Descriptor.StorageKind == EEditorBulkDataStorageKind::External
						? EPackageBulkDataPlacement::External : EPackageBulkDataPlacement::Inline,
					.LogicalSize = Descriptor.LogicalByteCount,
					.StoredSize = Descriptor.StoredByteCount,
					.SegmentOffset = Descriptor.SegmentOffset,
					.Alignment = Descriptor.Alignment,
					.ContentId = Descriptor.ContentHash});
			return Prepare(PackagePath, FSharedByteBuffer::Take(std::move(Main)),
				{Header.BulkSegmentExtent, Header.BulkSegmentDigest}, Entries,
				MaximumRetainedBytes, Out, IsCancelled);
		}
		catch (const std::bad_alloc&)
		{
			return {EPreparedStatus::BudgetExceeded, "Package closure allocation failed."};
		}
	}

	auto FPreparedPackageResource::Prepare(
		const std::filesystem::path& PackagePath, FSharedByteBuffer ValidatedMain,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries, uint64 MaximumRetainedBytes,
		FPreparedPackageResource& Out, const std::function<bool()>& IsCancelled)
		-> FPreparedPackageResourceResult
	{
		if (IsCancelled && IsCancelled())
			return {EPreparedStatus::Cancelled, "Package closure preparation was cancelled."};
		if (ValidatedMain.IsEmpty() || PackagePath.empty())
			return {EPreparedStatus::InvalidClosure, "A validated main package is required."};
		if (ValidatedMain.GetSize() > MaximumRetainedBytes
			|| Summary.Extent > MaximumRetainedBytes - ValidatedMain.GetSize()
			|| Summary.Extent > std::numeric_limits<size_t>::max())
			return {EPreparedStatus::BudgetExceeded, "Package closure exceeds the retained byte budget."};
		std::string Error;
		if (!ValidatePackageBulkDataMetadata(Summary, Entries, &Error))
			return {EPreparedStatus::InvalidClosure, std::move(Error)};
		try
		{
			FPreparedPackageResource Candidate;
			Candidate.MainPath = PackagePath;
			Candidate.MainDigest = FXxHash128::HashBuffer(ValidatedMain);
			Candidate.MainBytes = std::move(ValidatedMain);
			Candidate.BulkExtent = Summary.Extent;
			Candidate.BulkDigest = Summary.Digest;
			if (auto Check = CheckSnapshotFile(PackagePath, Candidate.MainBytes.GetSize(),
				Candidate.MainDigest, IsCancelled); !Check) return Check;
			if (Summary.Extent != 0)
			{
				auto BulkPath = PackagePath;
				BulkPath.replace_extension(".dbulk");
				FFileHelper::FFileIoError FileError;
				auto File = FFileHelper::OpenRead(BulkPath, &FileError);
				if (!File) return {EPreparedStatus::IoError, FileError.ToString()};
				if (File->GetSize() != Summary.Extent)
					return {EPreparedStatus::InvalidClosure, "Package bulk extent does not match main metadata."};
				FByteBuffer Bytes(static_cast<size_t>(Summary.Extent));
				for (uint64 Offset = 0; Offset < Summary.Extent;)
				{
					if (IsCancelled && IsCancelled())
						return {EPreparedStatus::Cancelled, "Package closure preparation was cancelled."};
					auto Chunk = std::span(Bytes).subspan(static_cast<size_t>(Offset),
						static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, Summary.Extent - Offset)));
					if (!File->ReadAt(Offset, Chunk, &FileError))
						return {EPreparedStatus::IoError, FileError.ToString()};
					Offset += Chunk.size();
				}
				if (!ValidatePackageBulkDataSegment(Summary, Entries, Bytes, &Error))
					return {EPreparedStatus::InvalidClosure, std::move(Error)};
				Candidate.BulkResource = std::make_shared<FSnapshotPackageResource>(
					FSharedByteBuffer::Take(std::move(Bytes)));
			}
			if (auto Check = Candidate.Revalidate(IsCancelled); !Check) return Check;
			Out = std::move(Candidate);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return {EPreparedStatus::BudgetExceeded, "Package closure allocation failed."};
		}
	}

	auto FPreparedPackageResource::Revalidate(const std::function<bool()>& IsCancelled) const
		-> FPreparedPackageResourceResult
	{
		if (IsCancelled && IsCancelled())
			return {EPreparedStatus::Cancelled, "Package closure validation was cancelled."};
		if (MainBytes.IsEmpty())
			return {EPreparedStatus::InvalidClosure, "No prepared package closure exists."};
		if (auto Check = CheckSnapshotFile(MainPath, MainBytes.GetSize(), MainDigest, IsCancelled); !Check)
			return Check;
		auto BulkPath = MainPath;
		BulkPath.replace_extension(".dbulk");
		if (BulkExtent != 0)
		{
			if (auto Check = CheckSnapshotFile(BulkPath, BulkExtent, BulkDigest, IsCancelled); !Check)
				return Check;
		}
		else
		{
			std::error_code Error;
			const bool bExists = std::filesystem::exists(BulkPath, Error);
			if (Error) return {EPreparedStatus::IoError, Error.message()};
			if (bExists) return {EPreparedStatus::InvalidClosure, "Package has an undeclared bulk companion."};
		}
		// Detect main publication while checking the companion. The captured bulk
		// remains stable even if the caller later receives Stale and retries.
		return CheckSnapshotFile(MainPath, MainBytes.GetSize(), MainDigest, IsCancelled);
	}

	auto FPackageResourceRequest::IsReady() const -> bool
	{
		if (!State) return true;
		std::lock_guard Lock(State->Mutex);
		if (State->bBinding) return false;
		const auto Completion = State->Completion();
		return !Completion.IsValid() || Completion.IsReady();
	}

	auto FPackageResourceRequest::Cancel() -> void
	{
		if (!State) return;
		State->bCancelled.store(true, std::memory_order_release);
		Tasks::FTaskCompletion Completion;
		{
			std::lock_guard Lock(State->Mutex);
			if (!State->bBinding) Completion = State->Completion();
		}
		if (State->OnCancel) State->OnCancel();
		if (Completion.IsValid()) Tasks::Cancel(Completion);
	}

	namespace
	{
		auto ReadCompletedPackageTask(const Tasks::TSharedTask<FPackageResourceReadResult>& Task)
			-> FPackageResourceReadResult
		{
			if (Task.GetState() == ETaskState::Succeeded) return Task.GetResult();
			if (Task.GetState() == ETaskState::Canceled)
				return Result(EPackageResourceReadStatus::Cancelled, "Package request task was cancelled.");
			return Result(EPackageResourceReadStatus::IoError, "Package request task failed.");
		}
	}

	auto FPackageResourceRequest::Wait() const -> FPackageResourceReadResult
	{
		if (!State) return Result(EPackageResourceReadStatus::Retired, "Package request is invalid.");
		Tasks::FTaskCompletion Completion;
		{
			std::unique_lock Lock(State->Mutex);
			State->Bound.wait(Lock, [&] { return !State->bBinding; });
			Completion = State->Completion();
		}
		if (Completion.IsValid() && Tasks::Wait(Completion).WaitStatus != ETaskWaitStatus::Completed)
			return Result(EPackageResourceReadStatus::IoError,
				"Package request wait was rejected; the request outcome is unchanged.");
		if (const auto* Immediate = std::get_if<FPackageResourceReadResult>(&State->Result)) return *Immediate;
		return ReadCompletedPackageTask(std::get<AssetPrivate::FPackageResourceRequestState::FSharedResult>(State->Result));
	}

	auto FPackageResourceRequest::Completed(FPackageResourceReadResult InResult)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>();
		State->EstimatedBytes += InResult.Buffer.GetSize() + InResult.Message.size();
		State->Result = std::move(InResult);
		return FPackageResourceRequest(std::move(State));
	}

	auto FPackageResourceRequest::Transform(
		FPackageResourceRequest Input,
		std::function<FPackageResourceReadResult(FPackageResourceReadResult)> Function)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>(true);
		if (Input.State)
		{
			std::unique_lock Lock(Input.State->Mutex);
			Input.State->Bound.wait(Lock, [&Input] { return !Input.State->bBinding; });
			State->Lifetime = Input.State->Lifetime;
			State->EstimatedBytes = Input.State->EstimatedBytes;
		}
		State->OnCancel = [Input]() mutable { Input.Cancel(); };
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = "PackageResource.Transform";
		Options.Attribution = PackageResourceAttribution();
		Options.EstimatedResultBytes = State->EstimatedBytes;
		if (Input.State && std::holds_alternative<AssetPrivate::FPackageResourceRequestState::FSharedResult>(Input.State->Result))
		{
			State->Bind(Tasks::ThenCompleted(std::get<AssetPrivate::FPackageResourceRequestState::FSharedResult>(Input.State->Result),
				Tasks::ETaskExecutor::Worker, Options,
				[Function = std::move(Function)](const Tasks::TSharedTask<FPackageResourceReadResult>& Task) mutable {
					return Function(ReadCompletedPackageTask(Task));
				}));
		}
		else
		{
			State->Lifetime = std::make_shared<AssetPrivate::FPackageTaskLifetime>();
			Tasks::FTaskGroup Group(State->Lifetime->Scope.GetToken());
			State->Bind(Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options,
				[Input = std::move(Input), Function = std::move(Function)]() mutable { return Function(Input.Wait()); }));
		}
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
		State->Lifetime = std::make_shared<AssetPrivate::FPackageTaskLifetime>();
		State->EstimatedBytes += Size;
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
		Tasks::FTaskGroup Group(State->Lifetime->Scope.GetToken());
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = "PackageResource.ReadRange";
		Options.Attribution = PackageResourceAttribution();
		Options.EstimatedResultBytes = State->EstimatedBytes;
		State->Bind(Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, Options,
			[Self = std::move(Self), State, Offset, Size](Tasks::FTaskContext& Context) {
				if (Context.GetCancellationToken().IsCancellationRequested()
					|| State->bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				return Self->ReadRangeImpl(Offset, Size, State->bCancelled);
			}));
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

	auto CreateOwnedPackageResource(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FByteView Segment,
		FPackageResourceHandle& OutHandle,
		std::string* OutError) -> bool
	{
		OutHandle.reset();
		if (!ValidatePackageBulkDataMetadata(Summary, Entries, OutError)) return false;
		if (Segment.size() != Summary.Extent)
		{
			if (OutError) *OutError = "Owned package bulk segment extent does not match its bytes.";
			return false;
		}
		// Validate the private allocation that subsequent reads will actually use.
		FSharedByteBuffer Bytes = FSharedByteBuffer::Copy(Segment);
		if (!ValidatePackageBulkDataSegment(Summary, Entries, Bytes.GetBytes(), OutError)) return false;
		OutHandle = std::make_shared<FOwnedPackageResource>(std::move(Bytes));
		return true;
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
