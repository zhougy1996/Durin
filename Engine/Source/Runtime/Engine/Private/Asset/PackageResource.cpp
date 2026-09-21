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
			std::atomic_bool bCancelled = false;
			std::function<void()> OnCancel;
			std::shared_ptr<FPackageTaskLifetime> Lifetime;

			auto Completion() const -> Tasks::FTaskCompletion
			{
				if (const auto* Task = std::get_if<FSharedResult>(&Result)) return Task->GetCompletion();
				return {};
			}
			auto Bind(Tasks::TTask<FPackageResourceReadResult> Task) -> void
			{
				std::variant<FPackageResourceReadResult, FSharedResult> Published = Tasks::Share(std::move(Task));
				Tasks::FTaskCompletion CompletionToCancel;
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

		auto ValidateLoosePackageGeneration(
			const std::filesystem::path& Path,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries,
			FPackageResourceReadStats& Stats) -> FPackageGenerationResult
		{
			auto RejectBulk = [&](FPackageBulkDataError Error) {
				Error.Summary = Summary;
				return std::unexpected(FPackageGenerationError{FPackageBulkValidationFailure{Path, std::move(Error)}});
			};
			auto File = FFileHelper::OpenRead(Path);
			if (!File)
			{
				return std::unexpected(FPackageGenerationError{std::move(File.error())});
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
					return std::unexpected(FPackageGenerationError{std::move(Read.error())});
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
					return Result(EPackageResourceReadStatus::Cancelled, {.Reason = EPackageResourceReadReason::Cancelled});
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
					return Result(EPackageResourceReadStatus::Cancelled, {.Reason = EPackageResourceReadReason::Cancelled});
				const std::array Paths{SegmentPath};
				auto Access = FPackageFileAccess::TryAcquire(Paths, false);
				if (!Access || IsRetired()) return Result(EPackageResourceReadStatus::Retired, {.Reason = EPackageResourceReadReason::PackageBusy});
				auto Reject = [&](EPackageResourceReadStatus Status, EPackageResourceReadReason Reason,
					uint64 Actual = 0, uint64 Expected = 0, std::error_code SystemError = {}, uint32 StreamState = 0) {
					return Result(Status, {.Reason = Reason, .Path = SegmentPath, .Offset = Offset,
						.Size = Size, .Extent = GetSegmentExtent(), .Actual = Actual, .Expected = Expected,
						.SystemError = SystemError, .StreamState = StreamState});
				};
				std::error_code Error;
				const uint64 BeforeSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error)
					return Reject(Error == std::errc::no_such_file_or_directory
						? EPackageResourceReadStatus::MissingSegment : EPackageResourceReadStatus::IoError,
						EPackageResourceReadReason::QuerySize, 0, 0, Error);
				if (BeforeSize != GetSegmentExtent())
					return Reject(EPackageResourceReadStatus::TruncatedSegment,
						EPackageResourceReadReason::ChangedBeforeRead, BeforeSize, GetSegmentExtent());
				std::ifstream Stream(SegmentPath, std::ios::binary);
				if (!Stream.is_open())
					return Reject(EPackageResourceReadStatus::IoError, EPackageResourceReadReason::Open,
						0, 0, {}, static_cast<uint32>(Stream.rdstate()));
				Stream.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
				if (!Stream)
					return Reject(EPackageResourceReadStatus::IoError, EPackageResourceReadReason::Seek,
						0, 0, {}, static_cast<uint32>(Stream.rdstate()));
				FByteBuffer Bytes(static_cast<size_t>(Size));
				if (Size != 0)
					Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size));
				if (Stream.gcount() != static_cast<std::streamsize>(Size))
					return Reject(EPackageResourceReadStatus::TruncatedSegment, EPackageResourceReadReason::ShortRead,
						static_cast<uint64>(Stream.gcount()), Size, {}, static_cast<uint32>(Stream.rdstate()));
				if (bCancelled.load(std::memory_order_acquire))
					return Reject(EPackageResourceReadStatus::Cancelled, EPackageResourceReadReason::Cancelled);
				const uint64 AfterSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error || AfterSize != BeforeSize)
					return Reject(EPackageResourceReadStatus::TruncatedSegment, EPackageResourceReadReason::ChangedDuringRead,
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
		using EPreparedReason = EPreparedPackageResourceReason;

		auto CheckSnapshotFile(const std::filesystem::path& Path, uint64 Extent,
			FXxHash128 Digest, const std::function<bool()>& IsCancelled)
			-> FPreparedPackageResourceResult
		{
			auto File = FFileHelper::OpenRead(Path);
			if (!File) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(File.error())});
			if ((*File)->GetSize() != Extent)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Stale, .Reason = EPreparedReason::ExtentChanged, .Path = Path, .Actual = (*File)->GetSize(), .Expected = Extent});
			std::array<std::byte, PackageValidationScratchBytes> Scratch{};
			FXxHash128Builder Hash;
			for (uint64 Offset = 0; Offset < Extent;)
			{
				if (IsCancelled && IsCancelled())
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = Path});
				auto Bytes = std::span(Scratch).first(static_cast<size_t>(
					std::min<uint64>(Scratch.size(), Extent - Offset)));
				if (auto Read = (*File)->ReadAt(Offset, Bytes); !Read)
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(Read.error())});
				Hash.Update(Bytes);
				Offset += Bytes.size();
			}
			const auto ActualDigest = Hash.Finalize();
			if (ActualDigest != Digest)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Stale, .Reason = EPreparedReason::DigestChanged, .Path = Path, .ActualDigest = ActualDigest, .ExpectedDigest = Digest});
			return {};
		}

		auto CompleteRetired() -> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::Retired, {.Reason = EPackageResourceReadReason::Retired}));
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
		if (!Access) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Stale, .Reason = EPreparedReason::PackageBusy, .Path = PackagePath});
		if (IsCancelled && IsCancelled())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = PackagePath});
		if (!LogicalPath.IsValid())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::InvalidLogicalPath, .Path = PackagePath});
		try
		{
			auto File = FFileHelper::OpenRead(PackagePath);
			if (!File) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(File.error())});
			const uint64 MainSize = (*File)->GetSize();
			if (MainSize > MaximumRetainedBytes || MainSize > std::numeric_limits<size_t>::max())
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::BudgetExceeded, .Reason = EPreparedReason::MainBudget, .Path = PackagePath, .MainBytes = MainSize, .MaximumBytes = MaximumRetainedBytes});
			FByteBuffer Main(static_cast<size_t>(MainSize));
			for (uint64 Offset = 0; Offset < MainSize;)
			{
				if (IsCancelled && IsCancelled())
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = PackagePath});
				auto Chunk = std::span(Main).subspan(static_cast<size_t>(Offset),
					static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, MainSize - Offset)));
				if (auto Read = (*File)->ReadAt(Offset, Chunk); !Read)
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(Read.error())});
				Offset += Chunk.size();
			}
			(*File).reset();
			auto BulkPath = PackagePath;
			BulkPath.replace_extension(".dbulk");
			std::error_code Error;
			uint64 BulkSize = 0;
			const bool bHasBulk = std::filesystem::exists(BulkPath, Error);
			if (!Error && bHasBulk) BulkSize = std::filesystem::file_size(BulkPath, Error);
			if (Error) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileSystem,
				.Cause = FFileError{EFileOperation::Inspect, Error, BulkPath}});
			if (BulkSize > MaximumRetainedBytes - MainSize)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::BudgetExceeded, .Reason = EPreparedReason::ClosureBudget, .Path = PackagePath, .MainBytes = MainSize, .BulkBytes = BulkSize, .MaximumBytes = MaximumRetainedBytes});
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (auto Result = AssetPrivate::ResolveAssetPackageReader(Main, Codec); !Result)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::ResolveCodec, .Path = PackagePath, .Cause = std::make_shared<const FAssetReadError>(AssetReadErrorFromResult(Result))});
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Main, .PackagePath = LogicalPath,
				.PhysicalPackageBytes = MainSize, .PhysicalBulkBytes = BulkSize,
				.bResourceBackedBulk = true};
			FAssetPackageHeader Header;
			if (auto Result = Codec->ReadHeader(Context, Header); !Result)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::ReadHeader, .Path = PackagePath, .Cause = std::make_shared<const FAssetReadError>(AssetReadErrorFromResult(Result))});
			FAssetPackageInspection Inspection;
			if (auto Result = Codec->Inspect(Context, Inspection); !Result)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::Inspect, .Path = PackagePath, .Cause = std::make_shared<const FAssetReadError>(AssetReadErrorFromResult(Result))});
			std::vector<FPackageBulkStorageDescriptor> Descriptors;
			if (auto Storage = InspectEditorBulkDataStorageDescriptors(Inspection); !Storage)
				return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure,
					.Reason = EPreparedReason::BulkStorage, .Path = PackagePath, .Cause = Storage.error()});
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
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::BudgetExceeded, .Reason = EPreparedReason::Allocation, .Path = PackagePath});
		}
	}

	auto FPreparedPackageResource::Prepare(
		const std::filesystem::path& PackagePath, FSharedByteBuffer ValidatedMain,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries, uint64 MaximumRetainedBytes, const std::function<bool()>& IsCancelled)
		-> std::expected<FPreparedPackageResource, FPreparedPackageResourceError>
	{
		auto Access = FPackageFileAccess::TryReadPackage(PackagePath);
		if (!Access) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Stale, .Reason = EPreparedReason::PackageBusy, .Path = PackagePath});
		if (IsCancelled && IsCancelled())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = PackagePath});
		if (ValidatedMain.IsEmpty() || PackagePath.empty())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::InvalidMain, .Path = PackagePath});
		if (ValidatedMain.GetSize() > MaximumRetainedBytes
			|| Summary.Extent > MaximumRetainedBytes - ValidatedMain.GetSize()
			|| Summary.Extent > std::numeric_limits<size_t>::max())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::BudgetExceeded, .Reason = EPreparedReason::ClosureBudget, .Path = PackagePath, .MainBytes = ValidatedMain.GetSize(), .BulkBytes = Summary.Extent, .MaximumBytes = MaximumRetainedBytes});
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation)
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::BulkValidation, .Path = PackagePath, .Cause = Validation.error()});
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
				if (!File) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(File.error())});
				if ((*File)->GetSize() != Summary.Extent)
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::BulkExtent, .Path = BulkPath, .Actual = (*File)->GetSize(), .Expected = Summary.Extent});
				FByteBuffer Bytes(static_cast<size_t>(Summary.Extent));
				for (uint64 Offset = 0; Offset < Summary.Extent;)
				{
					if (IsCancelled && IsCancelled())
						return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = PackagePath});
					auto Chunk = std::span(Bytes).subspan(static_cast<size_t>(Offset),
						static_cast<size_t>(std::min<uint64>(PackageValidationScratchBytes, Summary.Extent - Offset)));
					if (auto Read = (*File)->ReadAt(Offset, Chunk); !Read)
						return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileIo, .Cause = std::move(Read.error())});
					Offset += Chunk.size();
				}
				if (auto Validation = ValidatePackageBulkDataSegment(Summary, Entries, Bytes); !Validation)
					return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::BulkValidation, .Path = PackagePath, .Cause = Validation.error()});
				Candidate.BulkResource = std::make_shared<FSnapshotPackageResource>(
					FSharedByteBuffer::Take(std::move(Bytes)));
			}
			if (auto Check = Candidate.Revalidate(IsCancelled); !Check) return std::unexpected(std::move(Check.error()));
			return Candidate;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::BudgetExceeded, .Reason = EPreparedReason::Allocation, .Path = PackagePath});
		}
	}

	auto FPreparedPackageResource::Revalidate(const std::function<bool()>& IsCancelled) const
		-> FPreparedPackageResourceResult
	{
		auto Access = FPackageFileAccess::TryReadPackage(MainPath);
		if (!Access) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Stale, .Reason = EPreparedReason::PackageBusy, .Path = MainPath});
		if (IsCancelled && IsCancelled())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::Cancelled, .Reason = EPreparedReason::Cancelled, .Path = MainPath});
		if (MainBytes.IsEmpty())
			return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::EmptyPrepared, .Path = MainPath});
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
			if (Error) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::IoError, .Reason = EPreparedReason::FileSystem,
				.Cause = FFileError{EFileOperation::Inspect, Error, BulkPath}});
			if (bExists) return std::unexpected(FPreparedPackageResourceError{.Code = EPreparedStatus::InvalidClosure, .Reason = EPreparedReason::UndeclaredBulk, .Path = BulkPath});
		}
		// Detect main publication while checking the companion. The captured bulk
		// remains stable even if the caller later receives Stale and retries.
		return CheckSnapshotFile(MainPath, MainBytes.GetSize(), MainDigest, IsCancelled);
	}

	auto FormatPreparedPackageResourceError(const FPreparedPackageResourceError& Error) -> std::string
	{
		if (const auto* Cause = std::get_if<FFileError>(&Error.Cause)) return Cause->ToString();
		if (const auto* Cause = std::get_if<std::shared_ptr<const FAssetReadError>>(&Error.Cause); Cause && *Cause) return (*Cause)->Message;
		if (const auto* Cause = std::get_if<FPackageBulkDataError>(&Error.Cause)) return FormatPackageBulkDataError(*Cause);
		if (const auto* Cause = std::get_if<FEditorBulkDataStorageError>(&Error.Cause)) return FormatEditorBulkDataStorageError(*Cause);
		switch (Error.Reason)
		{
		case EPreparedReason::None: return {};
		case EPreparedReason::PackageBusy: return "Package output is being written.";
		case EPreparedReason::Cancelled: return "Package closure operation was cancelled.";
		case EPreparedReason::InvalidLogicalPath: return "A valid logical package identity is required.";
		case EPreparedReason::InvalidMain: return "A validated main package is required.";
		case EPreparedReason::EmptyPrepared: return "No prepared package closure exists.";
		case EPreparedReason::MainBudget: return "Package main file exceeds the retained byte budget.";
		case EPreparedReason::ClosureBudget: return "Package closure exceeds the retained byte budget.";
		case EPreparedReason::Allocation: return "Package closure allocation failed.";
		case EPreparedReason::FileSystem: return "Could not inspect package storage.";
		case EPreparedReason::ExtentChanged: return "Package closure extent changed.";
		case EPreparedReason::DigestChanged: return "Package closure content changed.";
		case EPreparedReason::BulkExtent: return "Package bulk extent does not match main metadata.";
		case EPreparedReason::UndeclaredBulk: return "Package has an undeclared bulk companion.";
		default: return "Package closure validation failed.";
		}
	}

	auto FormatPackageResourceReadError(const FPackageResourceReadResult& Result) -> std::string
	{
		if (Result) return {};
		switch (Result.error().Reason)
		{
		case EPackageResourceReadReason::Cancelled: return "Package range request was cancelled.";
		case EPackageResourceReadReason::Retired: return "Package resource is retired.";
		case EPackageResourceReadReason::InvalidRequest: return "Package request is invalid.";
		case EPackageResourceReadReason::InvalidRange: return "Package resource range is invalid.";
		case EPackageResourceReadReason::PackageBusy: return "Package output is being written.";
		case EPackageResourceReadReason::QuerySize: return "Package bulk segment cannot be opened.";
		case EPackageResourceReadReason::ChangedBeforeRead: return "Package bulk segment extent changed after registration.";
		case EPackageResourceReadReason::Open: return "Package bulk segment range cannot be opened.";
		case EPackageResourceReadReason::Seek: return "Package bulk segment range seek failed.";
		case EPackageResourceReadReason::ShortRead: return "Package bulk segment range is truncated.";
		case EPackageResourceReadReason::ChangedDuringRead: return "Package bulk segment changed during a range read.";
		case EPackageResourceReadReason::TaskCancelled: return "Package request task was cancelled.";
		case EPackageResourceReadReason::TaskFailed: return "Package request task failed.";
		case EPackageResourceReadReason::WaitRejected: return "Package request wait was rejected; the request outcome is unchanged.";
		case EPackageResourceReadReason::MissingSource: return "Editor bulk payload has no memory or package source.";
		case EPackageResourceReadReason::ContentMismatch: return "Editor bulk package range does not match its content identity.";
		case EPackageResourceReadReason::LogicalSizeMismatch: return "Bulk data logical size does not match the read.";
		case EPackageResourceReadReason::BulkUnavailable: return "Bulk data is empty, loading, or write locked.";
		case EPackageResourceReadReason::ReloadUnavailable: return "Bulk data cannot reload in its current state.";
		case EPackageResourceReadReason::None: break;
		}
		return "Package resource read failed.";
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
				return Result(EPackageResourceReadStatus::Cancelled, {.Reason = EPackageResourceReadReason::TaskCancelled, .TaskState = Task.GetState()});
			return Result(EPackageResourceReadStatus::IoError, {.Reason = EPackageResourceReadReason::TaskFailed, .TaskState = Task.GetState()});
		}
	}

	auto FPackageResourceRequest::Wait() const -> FPackageResourceReadResult
	{
		if (!State) return Result(EPackageResourceReadStatus::Retired, {.Reason = EPackageResourceReadReason::InvalidRequest});
		Tasks::FTaskCompletion Completion;
		{
			std::unique_lock Lock(State->Mutex);
			State->Bound.wait(Lock, [&] { return !State->bBinding; });
			Completion = State->Completion();
		}
		if (Completion.IsValid())
		{
			const auto Wait = Tasks::Wait(Completion);
			if (Wait.WaitStatus != ETaskWaitStatus::Completed)
				return Result(EPackageResourceReadStatus::IoError,
					{.Reason = EPackageResourceReadReason::WaitRejected, .WaitStatus = Wait.WaitStatus});
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
			State->Lifetime = Input.State->Lifetime;
		}
		State->OnCancel = [Input]() mutable { Input.Cancel(); };
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = "PackageResource.Transform";
		Options.Attribution = PackageResourceAttribution();
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
			State->Bind(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options,
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
				EPackageResourceReadStatus::InvalidRange, {.Reason = EPackageResourceReadReason::InvalidRange, .Offset = Offset, .Size = Size, .Extent = SegmentExtent}));

		auto State = std::make_shared<AssetPrivate::FPackageResourceRequestState>(true);
		State->Lifetime = std::make_shared<AssetPrivate::FPackageTaskLifetime>();
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
		State->Bind(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::BlockingIO, Options,
			[Self = std::move(Self), State, Offset, Size](Tasks::FTaskContext& Context) {
				if (Context.GetCancellationToken().IsCancellationRequested()
					|| State->bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, {.Reason = EPackageResourceReadReason::Cancelled});
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
		auto Generation = [](const std::optional<FPackageGenerationError>& Cause) {
			if (!Cause) return std::string{};
			return std::visit([](const auto& Failure) -> std::string {
				if constexpr (std::is_same_v<std::decay_t<decltype(Failure)>, FFileError>)
					return Failure.ToString();
				else return std::format("{}: {}", Failure.Path.generic_string(), FormatPackageBulkDataError(Failure.Error));
			}, *Cause);
		};
		switch (Error.Code)
		{
		case EPackageResourceRegistrationError::None: return {};
		case EPackageResourceRegistrationError::PackageBusy: return "Package output is being written.";
		case EPackageResourceRegistrationError::EmptySegment: return "Loose bulk registration requires a nonempty segment.";
		case EPackageResourceRegistrationError::InvalidMetadata:
			return Error.BulkCause ? FormatPackageBulkDataError(*Error.BulkCause) : "Package bulk metadata is invalid.";
		case EPackageResourceRegistrationError::ShuttingDown: return "Package resource manager is shut down.";
		case EPackageResourceRegistrationError::InvalidGeneration:
			return "Loose package bulk segment does not match the package generation: " + Generation(Error.PrimaryCause);
		}
		return "Package resource registration failed.";
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
		if (!Access) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::PackageBusy, .Path = PackagePath});
		if (Summary.Extent == 0) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::EmptySegment, .Path = PackagePath});
		if (auto Validation = ValidatePackageBulkDataMetadata(Summary, Entries); !Validation)
			return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::InvalidMetadata, .Path = PackagePath, .BulkCause = Validation.error()});
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown) return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::ShuttingDown, .Path = PackagePath});
		}
		std::filesystem::path SegmentPath = PackagePath;
		SegmentPath.replace_extension(".dbulk");
		FPackageResourceReadStats ValidationStats;
		auto Primary = ValidateLoosePackageGeneration(SegmentPath, Summary, Entries, ValidationStats);
		if (!Primary)
			return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::InvalidGeneration, .Path = PackagePath,
				.PrimaryCause = Primary.error()});

		auto Resource = std::make_shared<FLoosePackageResource>(
			SegmentPath, Summary.Extent, ValidationStats);
		FPackageResourceHandle Previous;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown)
			{
				return std::unexpected(FPackageResourceRegistrationError{.Code = EPackageResourceRegistrationError::ShuttingDown, .Path = PackagePath});
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
