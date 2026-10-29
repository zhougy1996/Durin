#include "Misc/PackageWriter.h"
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
		struct FPackageResourceRequestState
		{
			using FSharedResult = Tasks::TTask<FPackageResourceReadResult>;
			explicit FPackageResourceRequestState(bool bInBinding = false) : bBinding(bInBinding) {}
			// Synchronizes registration with resource retirement until the immutable
			// task is bound. Result publication and all later waits belong to Core.
			mutable std::mutex Mutex;
			std::condition_variable Bound;
			bool bBinding = false;
			std::variant<FPackageResourceReadResult, FSharedResult> Result;
			std::atomic_bool bCancelled = false;
			std::function<void()> OnCancel;

			auto Completion() const -> Durin::Tasks::FTask
			{
				if (const auto* Task = std::get_if<FSharedResult>(&Result)) return *Task;
				return {};
			}
			auto Bind(Tasks::TTask<FPackageResourceReadResult> Task) -> void
			{
				std::variant<FPackageResourceReadResult, FSharedResult> Published = std::move(Task);
				Durin::Tasks::FTask CompletionToCancel;
				{
					std::lock_guard Lock(Mutex);
					Result = std::move(Published);
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

		auto Result(EPackageResourceReadStatus Status, FPackageResourceReadError Error = {})
			-> FPackageResourceReadResult
		{
			Error.Status = Status;
			return std::unexpected(std::move(Error));
		}

		auto FormatBulkFailure(const FPackageBulkDataError& Error) -> std::string
		{
			return std::format("{} Index: {}. Offset: {}. Actual: {}. Expected: {}. ActualDigest: {}. ExpectedDigest: {}.",
				FormatPackageBulkDataError(Error), Error.Index, Error.Offset, Error.Actual, Error.Expected,
				Error.ActualDigest.ToString(),
				(Error.Code == EPackageBulkDataError::FieldDigestMismatch && Error.Entry
					? Error.Entry->ContentId : Error.Summary.Digest).ToString());
		}

		auto ValidateLoosePackageGeneration(
			const std::filesystem::path& Path,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries,
			FPackageResourceReadStats& Stats) -> std::expected<void, std::string>
		{
			auto RejectBulk = [&](FPackageBulkDataError Error) {
				Error.Summary = Summary;
				return std::unexpected(std::format("{}: {}", Path.generic_string(), FormatBulkFailure(Error)));
			};
			auto File = FFileHelper::OpenRead(Path);
			if (!File)
			{
				return std::unexpected(File.error().ToString());
			}
			if ((*File)->GetSize() != Summary.Extent)
			{
				return RejectBulk({.Code = EPackageBulkDataError::ExtentMismatch,
					.Actual = (*File)->GetSize(), .Expected = Summary.Extent});
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
				if (auto Read = (*File)->ReadAt(Offset, Bytes); !Read)
				{
					return std::unexpected(Read.error().ToString());
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
						const auto Padding = Bytes.subspan(LocalOffset, PaddingBytes);
						if (const auto Bad = std::ranges::find_if(Padding,
								[](std::byte Byte) { return Byte != std::byte{0}; }); Bad != Padding.end())
						{
							return RejectBulk({.Code = EPackageBulkDataError::NonzeroPadding,
								.Index = EntryIndex, .Entry = Entry, .Actual = std::to_integer<uint8>(*Bad),
								.Offset = AbsoluteOffset + static_cast<uint64>(Bad - Padding.begin())});
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
						const auto Digest = FieldHash.Finalize();
						if (Digest != Entry.ContentId)
						{
							return RejectBulk({.Code = EPackageBulkDataError::FieldDigestMismatch,
								.Index = EntryIndex, .Entry = Entry, .ActualDigest = Digest});
						}
						FieldHash.Reset();
						++EntryIndex;
						AdvanceToExternal();
					}
				}
				Offset += Count;
			}
			const auto Digest = SegmentHash.Finalize();
			if (EntryIndex != Entries.size() || Digest != Summary.Digest)
			{
				return RejectBulk({.Code = EPackageBulkDataError::SegmentDigestMismatch,
					.Index = EntryIndex, .Actual = EntryIndex, .Expected = Entries.size(), .ActualDigest = Digest});
			}
			return {};
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
					return Result(EPackageResourceReadStatus::Cancelled, {.Message = "Package range request was cancelled."});
				return Bytes.MakeView(Offset, Size);
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
					return Result(EPackageResourceReadStatus::Cancelled, {.Message = "Package range request was cancelled."});
				const std::array Paths{SegmentPath};
				auto Access = FPackageFileAccess::TryAcquire(Paths, false);
				if (!Access || IsRetired()) return Result(EPackageResourceReadStatus::Retired, {.Message = "Package output is being written."});
				auto Reject = [&](EPackageResourceReadStatus Status, std::string_view Message,
					uint64 Actual = 0, uint64 Expected = 0, std::error_code SystemError = {}, uint32 StreamState = 0) {
					return Result(Status, {.Message = std::format(
						"{} Path: {}. Offset: {}. Size: {}. Extent: {}. Actual: {}. Expected: {}. System error: {}. Stream state: {}.",
						Message, SegmentPath.generic_string(), Offset, Size, GetSegmentExtent(), Actual, Expected, SystemError.message(), StreamState)});
				};
				std::error_code Error;
				const uint64 BeforeSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error)
					return Reject(Error == std::errc::no_such_file_or_directory
						? EPackageResourceReadStatus::MissingSegment : EPackageResourceReadStatus::IoError,
						"Package bulk segment cannot be opened.", 0, 0, Error);
				if (BeforeSize != GetSegmentExtent())
					return Reject(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment extent changed after registration.", BeforeSize, GetSegmentExtent());
				std::ifstream Stream(SegmentPath, std::ios::binary);
				if (!Stream.is_open())
					return Reject(EPackageResourceReadStatus::IoError, "Package bulk segment range cannot be opened.",
						0, 0, {}, static_cast<uint32>(Stream.rdstate()));
				Stream.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
				if (!Stream)
					return Reject(EPackageResourceReadStatus::IoError, "Package bulk segment range seek failed.",
						0, 0, {}, static_cast<uint32>(Stream.rdstate()));
				FByteBuffer Bytes(static_cast<size_t>(Size));
				if (Size != 0)
					Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size));
				if (Stream.gcount() != static_cast<std::streamsize>(Size))
					return Reject(EPackageResourceReadStatus::TruncatedSegment, "Package bulk segment range is truncated.",
						static_cast<uint64>(Stream.gcount()), Size, {}, static_cast<uint32>(Stream.rdstate()));
				if (bCancelled.load(std::memory_order_acquire))
					return Reject(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				const uint64 AfterSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error || AfterSize != BeforeSize)
					return Reject(EPackageResourceReadStatus::TruncatedSegment, "Package bulk segment changed during a range read.",
						Error ? 0 : AfterSize, BeforeSize, Error);
				return FSharedByteBuffer::Take(std::move(Bytes));
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
				return Bytes.MakeView(Offset, Size);
			}
			FSharedByteBuffer Bytes;
		};

		using EPreparedStatus = EPreparedPackageResourceError;

		auto PreparationFailed(EPreparedStatus Code, std::string Message)
			-> std::unexpected<FPreparedPackageResourceError>
		{
			return std::unexpected(FPreparedPackageResourceError{Code, std::move(Message)});
		}

		auto CheckSnapshotFile(const std::filesystem::path& Path, uint64 Extent,
			FXxHash128 Digest, const std::function<bool()>& IsCancelled)
			-> FPreparedPackageResourceResult
		{
			auto File = FFileHelper::OpenRead(Path);
			if (!File) return PreparationFailed(EPreparedStatus::IoError,
				File.error().ToString());
			if ((*File)->GetSize() != Extent)
				return PreparationFailed(EPreparedStatus::Stale,
					std::format("Package closure extent changed. Path: {}. Actual: {}. Expected: {}.", Path.generic_string(), (*File)->GetSize(), Extent));
			std::array<std::byte, PackageValidationScratchBytes> Scratch{};
			FXxHash128Builder Hash;
			for (uint64 Offset = 0; Offset < Extent;)
			{
				if (IsCancelled && IsCancelled())
					return PreparationFailed(EPreparedStatus::Cancelled,
						std::format("Package closure operation was cancelled. Path: {}.", Path.generic_string()));
				auto Bytes = std::span(Scratch).first(static_cast<size_t>(
					std::min<uint64>(Scratch.size(), Extent - Offset)));
				if (auto Read = (*File)->ReadAt(Offset, Bytes); !Read)
					return PreparationFailed(EPreparedStatus::IoError,
						Read.error().ToString());
				Hash.Update(Bytes);
				Offset += Bytes.size();
			}
			const auto ActualDigest = Hash.Finalize();
			if (ActualDigest != Digest)
				return PreparationFailed(EPreparedStatus::Stale,
					std::format("Package closure content changed. Path: {}. ActualDigest: {}. ExpectedDigest: {}.", Path.generic_string(), ActualDigest.ToString(), Digest.ToString()));
			return {};
		}

		auto CompleteRetired() -> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::Retired, {.Message = "Package resource is retired."}));
		}

		auto PackageResourceAttribution() -> FTaskAttribution
		{
			static const FTaskAttribution Attribution =
				RegisterTaskAttribution("Engine", "PackageResource");
			return Attribution;
		}
	}

	auto FPreparedPackageResource::Read(const FPackagePath& LogicalPath,
		const std::filesystem::path& PackagePath, uint64 MaximumRetainedBytes, const std::function<bool()>& IsCancelled)
		-> std::expected<FPreparedPackageResource, FPreparedPackageResourceError>
	{
		auto Access = FPackageFileAccess::TryReadPackage(PackagePath);
		if (!Access) return PreparationFailed(EPreparedStatus::Stale,
			std::format("Package output is being written. Path: {}.", PackagePath.generic_string()));
		if (IsCancelled && IsCancelled())
			return PreparationFailed(EPreparedStatus::Cancelled,
				std::format("Package closure operation was cancelled. Path: {}.", PackagePath.generic_string()));
		if (!LogicalPath.IsValid())
			return PreparationFailed(EPreparedStatus::InvalidClosure,
				std::format("A valid logical package identity is required. Path: {}.", PackagePath.generic_string()));
		try
		{
			auto File = FFileHelper::OpenRead(PackagePath);
			if (!File) return PreparationFailed(EPreparedStatus::IoError,
				File.error().ToString());
			const uint64 MainSize = (*File)->GetSize();
			if (MainSize > MaximumRetainedBytes || MainSize > std::numeric_limits<size_t>::max())
				return PreparationFailed(EPreparedStatus::BudgetExceeded,
					std::format("Package main file exceeds the retained byte budget. Path: {}. MainBytes: {}. MaximumBytes: {}.", PackagePath.generic_string(), MainSize, MaximumRetainedBytes));
			FByteBuffer Main(static_cast<size_t>(MainSize));
			for (uint64 Offset = 0; Offset < MainSize;)
			{
				if (IsCancelled && IsCancelled())
					return PreparationFailed(EPreparedStatus::Cancelled,
						std::format("Package closure operation was cancelled. Path: {}.", PackagePath.generic_string()));
				auto Chunk = std::span(Main).subspan(static_cast<size_t>(Offset),
					static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, MainSize - Offset)));
				if (auto Read = (*File)->ReadAt(Offset, Chunk); !Read)
					return PreparationFailed(EPreparedStatus::IoError,
						Read.error().ToString());
				Offset += Chunk.size();
			}
			(*File).reset();
			auto BulkPath = PackagePath;
			BulkPath.replace_extension(".dbulk");
			std::error_code Error;
			uint64 BulkSize = 0;
			const bool bHasBulk = std::filesystem::exists(BulkPath, Error);
			if (!Error && bHasBulk) BulkSize = std::filesystem::file_size(BulkPath, Error);
			if (Error) return PreparationFailed(EPreparedStatus::IoError,
				(FFileError{EFileOperation::Inspect, Error, BulkPath}).ToString());
			if (BulkSize > MaximumRetainedBytes - MainSize)
				return PreparationFailed(EPreparedStatus::BudgetExceeded,
					std::format("Package closure exceeds the retained byte budget. Path: {}. MainBytes: {}. BulkBytes: {}. MaximumBytes: {}.", PackagePath.generic_string(), MainSize, BulkSize, MaximumRetainedBytes));
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (auto Result = AssetPrivate::ResolveAssetPackageReader(Main, Codec); !Result)
				return PreparationFailed(EPreparedStatus::InvalidClosure,
					std::format("{} Path: {}.", Result.Message, PackagePath.generic_string()));
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Main, .PackagePath = LogicalPath,
				.PhysicalPackageBytes = MainSize, .PhysicalBulkBytes = BulkSize,
				.bResourceBackedBulk = true};
			FAssetPackageHeader Header;
			if (auto Result = Codec->ReadHeader(Context, Header); !Result)
				return PreparationFailed(EPreparedStatus::InvalidClosure,
					std::format("{} Path: {}.", Result.Message, PackagePath.generic_string()));
			FAssetPackageInspection Inspection;
			if (auto Result = Codec->Inspect(Context, Inspection); !Result)
				return PreparationFailed(EPreparedStatus::InvalidClosure,
					std::format("{} Path: {}.", Result.Message, PackagePath.generic_string()));
			std::vector<FPackageBulkStorageDescriptor> Descriptors;
			if (auto Storage = InspectEditorBulkDataStorageDescriptors(Inspection); !Storage)
				return PreparationFailed(EPreparedStatus::InvalidClosure,
					std::format("{} Path: {}.", FormatEditorBulkDataStorageError(Storage.error()), PackagePath.generic_string()));
			else { Descriptors = std::move(*Storage); }
			std::vector<FPackageBulkDataEntry> Entries;
			Entries.reserve(Descriptors.size());
			for (const auto& Descriptor : Descriptors)
				Entries.push_back({.FieldIndex = Entries.size() + 1,
					.Placement = Descriptor.StorageKind == EPackageBulkStorageKind::External
						? EPackageBulkDataPlacement::External : EPackageBulkDataPlacement::Inline,
					.LogicalSize = Descriptor.LogicalByteCount,
					.StoredSize = Descriptor.StoredByteCount,
					.SegmentOffset = Descriptor.SegmentOffset,
					.Alignment = Descriptor.Alignment,
					.ContentId = Descriptor.ContentHash});
			return Prepare(PackagePath, FSharedByteBuffer::Take(std::move(Main)),
				{Header.BulkSegmentExtent, Header.BulkSegmentDigest}, Entries,
				MaximumRetainedBytes, IsCancelled);
		}
		catch (const std::bad_alloc&)
		{
			return PreparationFailed(EPreparedStatus::BudgetExceeded,
				std::format("Package closure allocation failed. Path: {}.", PackagePath.generic_string()));
		}
	}

	auto FPreparedPackageResource::Prepare(
		const std::filesystem::path& PackagePath, FSharedByteBuffer ValidatedMain,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries, uint64 MaximumRetainedBytes, const std::function<bool()>& IsCancelled)
		-> std::expected<FPreparedPackageResource, FPreparedPackageResourceError>
	{
		auto Access = FPackageFileAccess::TryReadPackage(PackagePath);
		if (!Access) return PreparationFailed(EPreparedStatus::Stale,
			std::format("Package output is being written. Path: {}.", PackagePath.generic_string()));
		if (IsCancelled && IsCancelled())
			return PreparationFailed(EPreparedStatus::Cancelled,
				std::format("Package closure operation was cancelled. Path: {}.", PackagePath.generic_string()));
		if (ValidatedMain.IsEmpty() || PackagePath.empty())
			return PreparationFailed(EPreparedStatus::InvalidClosure,
				std::format("A validated main package is required. Path: {}.", PackagePath.generic_string()));
		if (ValidatedMain.GetSize() > MaximumRetainedBytes
			|| Summary.Extent > MaximumRetainedBytes - ValidatedMain.GetSize()
			|| Summary.Extent > std::numeric_limits<size_t>::max())
			return PreparationFailed(EPreparedStatus::BudgetExceeded,
				std::format("Package closure exceeds the retained byte budget. Path: {}. MainBytes: {}. BulkBytes: {}. MaximumBytes: {}.", PackagePath.generic_string(), ValidatedMain.GetSize(), Summary.Extent, MaximumRetainedBytes));
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation)
			return PreparationFailed(EPreparedStatus::InvalidClosure,
				std::format("{} Path: {}.", FormatBulkFailure(Validation.error()), PackagePath.generic_string()));
		try
		{
			FPreparedPackageResource Candidate;
			Candidate.MainPath = PackagePath;
			Candidate.MainDigest = FXxHash128::HashBuffer(ValidatedMain);
			Candidate.MainBytes = std::move(ValidatedMain);
			Candidate.BulkExtent = Summary.Extent;
			Candidate.BulkDigest = Summary.Digest;
			if (auto Check = CheckSnapshotFile(PackagePath, Candidate.MainBytes.GetSize(),
				Candidate.MainDigest, IsCancelled); !Check) return std::unexpected(std::move(Check.error()));
			if (Summary.Extent != 0)
			{
				auto BulkPath = PackagePath;
				BulkPath.replace_extension(".dbulk");
				auto File = FFileHelper::OpenRead(BulkPath);
				if (!File) return PreparationFailed(EPreparedStatus::IoError,
					File.error().ToString());
				if ((*File)->GetSize() != Summary.Extent)
					return PreparationFailed(EPreparedStatus::InvalidClosure,
						std::format("Package bulk extent does not match main metadata. Path: {}. Actual: {}. Expected: {}.", BulkPath.generic_string(), (*File)->GetSize(), Summary.Extent));
				FByteBuffer Bytes(static_cast<size_t>(Summary.Extent));
				for (uint64 Offset = 0; Offset < Summary.Extent;)
				{
					if (IsCancelled && IsCancelled())
						return PreparationFailed(EPreparedStatus::Cancelled,
							std::format("Package closure operation was cancelled. Path: {}.", PackagePath.generic_string()));
					auto Chunk = std::span(Bytes).subspan(static_cast<size_t>(Offset),
						static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, Summary.Extent - Offset)));
					if (auto Read = (*File)->ReadAt(Offset, Chunk); !Read)
						return PreparationFailed(EPreparedStatus::IoError,
							Read.error().ToString());
					Offset += Chunk.size();
				}
				if (auto Validation = ValidatePackageBulkDataSegment(Summary, Entries, Bytes); !Validation)
					return PreparationFailed(EPreparedStatus::InvalidClosure,
						std::format("{} Path: {}.", FormatBulkFailure(Validation.error()), PackagePath.generic_string()));
				Candidate.BulkResource = std::make_shared<FSnapshotPackageResource>(
					FSharedByteBuffer::Take(std::move(Bytes)));
			}
			if (auto Check = Candidate.Revalidate(IsCancelled); !Check) return std::unexpected(std::move(Check.error()));
			return Candidate;
		}
		catch (const std::bad_alloc&)
		{
			return PreparationFailed(EPreparedStatus::BudgetExceeded,
				std::format("Package closure allocation failed. Path: {}.", PackagePath.generic_string()));
		}
	}

	auto FPreparedPackageResource::Revalidate(const std::function<bool()>& IsCancelled) const
		-> FPreparedPackageResourceResult
	{
		auto Access = FPackageFileAccess::TryReadPackage(MainPath);
		if (!Access) return PreparationFailed(EPreparedStatus::Stale,
			std::format("Package output is being written. Path: {}.", MainPath.generic_string()));
		if (IsCancelled && IsCancelled())
			return PreparationFailed(EPreparedStatus::Cancelled,
				std::format("Package closure operation was cancelled. Path: {}.", MainPath.generic_string()));
		if (MainBytes.IsEmpty())
			return PreparationFailed(EPreparedStatus::InvalidClosure,
				std::format("No prepared package closure exists. Path: {}.", MainPath.generic_string()));
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
			if (Error) return PreparationFailed(EPreparedStatus::IoError,
				(FFileError{EFileOperation::Inspect, Error, BulkPath}).ToString());
			if (bExists) return PreparationFailed(EPreparedStatus::InvalidClosure,
				std::format("Package has an undeclared bulk companion. Path: {}.", BulkPath.generic_string()));
		}
		// Detect main publication while checking the companion. The captured bulk
		// remains stable even if the caller later receives Stale and retries.
		return CheckSnapshotFile(MainPath, MainBytes.GetSize(), MainDigest, IsCancelled);
	}

	auto FormatPreparedPackageResourceError(const FPreparedPackageResourceError& Error) -> std::string
	{
		return Error.Message.empty() ? "Package closure validation failed." : Error.Message;
	}

	auto FormatPackageResourceReadError(const FPackageResourceReadError& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.Message;
		switch (Error.Status)
		{
		case EPackageResourceReadStatus::Success: return {};
		case EPackageResourceReadStatus::InvalidRange: return "Package resource range is invalid.";
		case EPackageResourceReadStatus::MissingSegment: return "Package bulk segment is missing.";
		case EPackageResourceReadStatus::TruncatedSegment: return "Package bulk segment is truncated or changed.";
		case EPackageResourceReadStatus::SegmentDigestMismatch: return "Package bulk content does not match its identity.";
		case EPackageResourceReadStatus::Cancelled: return "Package resource read was cancelled.";
		case EPackageResourceReadStatus::Retired: return "Package resource is retired.";
		case EPackageResourceReadStatus::IoError: return "Package resource I/O failed.";
		}
		return "Package resource read failed.";
	}

	auto FormatPackageResourceReadError(const FPackageResourceReadResult& Result) -> std::string
	{
		return Result ? std::string{} : FormatPackageResourceReadError(Result.error());
	}

	auto FPackageResourceRequest::IsReady() const -> bool
	{
		if (!State) return true;
		std::lock_guard Lock(State->Mutex);
		if (State->bBinding) return false;
		const auto Completion = State->Completion();
		return !Completion.IsValid() || Completion.IsCompleted();
	}

	auto FPackageResourceRequest::Cancel() -> void
	{
		if (!State) return;
		State->bCancelled.store(true, std::memory_order_release);
		Durin::Tasks::FTask Completion;
		{
			std::lock_guard Lock(State->Mutex);
			if (!State->bBinding) Completion = State->Completion();
		}
		if (State->OnCancel) State->OnCancel();
		if (Completion.IsValid()) Tasks::Cancel(Completion);
	}

	namespace
	{
		auto ReadCompletedPackageTask(const Tasks::TTask<FPackageResourceReadResult>& Task)
			-> FPackageResourceReadResult
		{
			if (Task.GetState() == ETaskState::Succeeded) return Task.GetResult();
			if (Task.GetState() == ETaskState::Canceled)
				return Result(EPackageResourceReadStatus::Cancelled, {.Message = std::format("Package request task was cancelled. TaskState: {}.", static_cast<uint32>(Task.GetState()))});
			return Result(EPackageResourceReadStatus::IoError, {.Message = std::format("Package request task failed. TaskState: {}.", static_cast<uint32>(Task.GetState()))});
		}
	}

	auto FPackageResourceRequest::Wait(std::optional<double> TimeoutSeconds) const -> FPackageResourceReadResult
	{
		if (!State) return Result(EPackageResourceReadStatus::Retired, {.Message = "Package request is invalid."});
		if (TimeoutSeconds && (!std::isfinite(*TimeoutSeconds) || *TimeoutSeconds < 0.0))
			return Result(EPackageResourceReadStatus::IoError, {.Message = "Invalid package request wait timeout."});
		const auto Started = std::chrono::steady_clock::now();
		Durin::Tasks::FTask Completion;
		{
			std::unique_lock Lock(State->Mutex);
			if (TimeoutSeconds)
			{
				if (!State->Bound.wait_for(Lock, std::chrono::duration<double>(*TimeoutSeconds), [&] { return !State->bBinding; }))
					return Result(EPackageResourceReadStatus::IoError, {.Message = "Package request binding wait timed out; the request outcome is unchanged."});
			}
			else State->Bound.wait(Lock, [&] { return !State->bBinding; });
			Completion = State->Completion();
		}
		if (Completion.IsValid())
		{
			const auto Wait = TimeoutSeconds ? Tasks::Wait(Completion, std::max(0.0, *TimeoutSeconds
				- std::chrono::duration<double>(std::chrono::steady_clock::now() - Started).count())) : Tasks::Wait(Completion);
			if (Wait.WaitStatus != ETaskWaitStatus::Completed)
				return Result(EPackageResourceReadStatus::IoError,
					{.Message = std::format("Package request wait was rejected; the request outcome is unchanged. WaitStatus: {}.", static_cast<uint32>(Wait.WaitStatus))});
		}
		if (const auto* Immediate = std::get_if<FPackageResourceReadResult>(&State->Result)) return *Immediate;
		return ReadCompletedPackageTask(std::get<AssetPrivate::FPackageResourceRequestState::FSharedResult>(State->Result));
	}

	auto FPackageResourceRequest::Completed(FPackageResourceReadResult InResult)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>();
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
		}
		State->OnCancel = [Input]() mutable { Input.Cancel(); };
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = "PackageResource.Transform";
		Options.Attribution = PackageResourceAttribution();
		if (Input.State && std::holds_alternative<AssetPrivate::FPackageResourceRequestState::FSharedResult>(Input.State->Result))
		{
			State->Bind(Tasks::ThenCompleted(std::get<AssetPrivate::FPackageResourceRequestState::FSharedResult>(Input.State->Result),
				Tasks::ETaskExecutor::Worker, Options,
				[Function = std::move(Function)](const Tasks::TTask<FPackageResourceReadResult>& Task) mutable {
					return Function(ReadCompletedPackageTask(Task));
				}));
		}
		else
		{
			State->Bind(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options,
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
				EPackageResourceReadStatus::InvalidRange, {.Message = std::format("Package resource range is invalid. Offset: {}. Size: {}. Extent: {}.", Offset, Size, SegmentExtent)}));

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
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = "PackageResource.ReadRange";
		Options.Attribution = PackageResourceAttribution();
		State->Bind(Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, Options,
			[Self = std::move(Self), State, Offset, Size](const FTaskCancellationToken& Token) {
				if (Token.IsCancellationRequested()
					|| State->bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, {.Message = "Package range request was cancelled."});
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
		FByteView Segment) -> std::expected<FPackageResourceHandle, FPackageBulkDataError>
	{
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation) return std::unexpected(std::move(Validation.error()));
		if (Segment.size() != Summary.Extent)
		{
			return std::unexpected(FPackageBulkDataError{.Code = EPackageBulkDataError::ExtentMismatch, .Summary = Summary,
				.Actual = Segment.size(), .Expected = Summary.Extent});
		}
		// Validate the private allocation that subsequent reads will actually use.
		FSharedByteBuffer Bytes = FSharedByteBuffer::Copy(Segment);
		if (auto Validation = ValidatePackageBulkDataSegment(Summary, Entries, Bytes.GetBytes()); !Validation) return std::unexpected(std::move(Validation.error()));
		return std::make_shared<FOwnedPackageResource>(std::move(Bytes));
	}

	auto FormatPackageResourceRangeError(const FPackageResourceRangeError& Error) -> std::string
	{
		return Error.Code == EPackageResourceRangeError::None
			? std::string{} : "Package resource range is invalid or unsupported.";
	}

	auto ValidatePackageResourceRange(const FPackageResourceRange& Range,
		uint64 MaximumStoredSize) -> FPackageResourceRangeResult
	{
		const uint64 Extent = Range.Resource ? Range.Resource->GetSegmentExtent() : 0;
		auto Reject = [&](EPackageResourceRangeError Code) {
			return std::unexpected(FPackageResourceRangeError{.Code = Code,
				.SegmentOffset = Range.SegmentOffset, .StoredSize = Range.StoredSize,
				.SegmentExtent = Extent, .MaximumStoredSize = MaximumStoredSize,
				.StorageFlags = Range.StorageFlags, .Alignment = Range.Alignment});
		};
		if (!Range.Resource) return Reject(EPackageResourceRangeError::MissingResource);
		if (Range.StorageFlags != 0) return Reject(EPackageResourceRangeError::UnsupportedFlags);
		if (Range.StoredSize > MaximumStoredSize) return Reject(EPackageResourceRangeError::SizeLimit);
		if (Range.Alignment == 0 || Range.Alignment > 4096 || (Range.Alignment & (Range.Alignment - 1)) != 0)
			return Reject(EPackageResourceRangeError::InvalidAlignment);
		if (Range.SegmentOffset % Range.Alignment != 0) return Reject(EPackageResourceRangeError::MisalignedOffset);
		if (Range.SegmentOffset > Extent || Range.StoredSize > Extent - Range.SegmentOffset)
			return Reject(EPackageResourceRangeError::OutsideSegment);
		return {};
	}

	FPackageResourceManager::~FPackageResourceManager()
	{
		Shutdown();
	}

	auto FormatPackageResourceRegistrationError(const FPackageResourceRegistrationError& Error) -> std::string
	{
		return Error.Message.empty() ? "Package resource registration failed." : Error.Message;
	}

	auto FPackageResourceManager::RegisterLoosePackage(
		std::string LogicalPackageId,
		const std::filesystem::path& PackagePath,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries
	) -> FPackageResourceRegistrationResult
	{
		require(!LogicalPackageId.empty());
		auto Access = FPackageFileAccess::TryReadPackage(PackagePath);
		if (!Access) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::PackageBusy, .Message = std::format("Package output is being written. Path: {}.", PackagePath.generic_string())});
		if (Summary.Extent == 0) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::EmptySegment, .Message = std::format("Loose bulk registration requires a nonempty segment. Path: {}.", PackagePath.generic_string())});
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation)
			return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::InvalidMetadata, .Message = std::format("{} Path: {}.", FormatBulkFailure(Validation.error()), PackagePath.generic_string())});
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::ShuttingDown, .Message = std::format("Package resource manager is shut down. Path: {}.", PackagePath.generic_string())});
		}
		std::filesystem::path SegmentPath = PackagePath;
		SegmentPath.replace_extension(".dbulk");
		FPackageResourceReadStats ValidationStats;
		auto Primary = ValidateLoosePackageGeneration(SegmentPath, Summary, Entries, ValidationStats);
		if (!Primary)
			return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::InvalidGeneration, .Message = std::format("{} Path: {}.", Primary.error(), PackagePath.generic_string())});

		auto Resource = std::make_shared<FLoosePackageResource>(
			SegmentPath, Summary.Extent, ValidationStats);
		FPackageResourceHandle Previous;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown)
			{
				return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::ShuttingDown, .Message = std::format("Package resource manager is shut down. Path: {}.", PackagePath.generic_string())});
			}
			auto& Slot = Resources[std::move(LogicalPackageId)];
			Previous = std::move(Slot);
			Slot = Resource;
		}
		if (Previous) Previous->Retire();
		return Resource;
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
