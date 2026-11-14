#include "Misc/PackageWriter.h"
#include "Misc/FileHelper.h"
#include <cwctype>

namespace Durin
{
	namespace
	{
		auto Failure(UnifiedError::FErrorId Id, std::string Message = {}, EPackageWriteState State = EPackageWriteState::NotCommitted) -> FPackageWriteResult
		{ return {.Error = UnifiedError::FError{Id, std::move(Message)}, .State = State}; }
		struct FFileAccessCount { uint32 Readers = 0; bool bWriter = false; };
		std::mutex FileAccessMutex;
		std::unordered_map<std::filesystem::path, FFileAccessCount> FileAccessCounts;
		auto AcquireWriteFiles(const std::vector<FPackageWriteFile>& Files) -> std::shared_ptr<FPackageFileAccess>
		{
			std::vector<std::filesystem::path> Paths;
			for (const auto& File : Files) Paths.push_back(File.Replacement.Destination);
			return FPackageFileAccess::TryAcquire(Paths, true);
		}
		class FFilePackageWriteOperation final : public IPackageWriteOperation
		{
			std::vector<FPackageWriteFile> Files;
			std::vector<FFilePublicationStamp> StagedStamps;
			std::vector<std::filesystem::path> OwnedStages;
			bool bStaged = false, bCommitted = false, bTerminal = false;
			FPackageWriteResult Result;
			std::shared_ptr<FPackageFileAccess> Access;
			auto Cleanup() -> void
			{
				std::error_code Ec;
				for (const auto& Path : OwnedStages) std::filesystem::remove(Path, Ec);
				OwnedStages.clear();
			}
			auto RecoveryFiles(FPackageWriteResult& Out) const -> void
			{
				for (const auto& File : Files)
					if (File.Replacement.bBackedUp) Out.RecoveryFiles.push_back(File.Replacement.Backup);
			}
		public:
			explicit FFilePackageWriteOperation(std::vector<FPackageWriteFile> InFiles)
				: Files(std::move(InFiles)) {}
			~FFilePackageWriteOperation() override
			{
				if (bCommitted && !bTerminal) (void)Rollback();
				Cleanup();
			}
			auto Stage(std::optional<UnifiedError::FError>* OutError) -> bool override
			{
				if (OutError) OutError->reset();
				if (bTerminal || bStaged || bCommitted)
				{
					if (OutError) *OutError = Result.Error ? Result.Error
						: std::optional{UnifiedError::FError{PackageWriteErrors::InvalidState, "Writer is no longer available for staging."}};
					return false;
				}
				auto Reject = [&](UnifiedError::FError Error) {
					bTerminal = true;
					Cleanup();
					Result.Error = std::move(Error);
					if (OutError) *OutError = Result.Error;
					return false;
				};
				std::unordered_set<std::filesystem::path> Paths;
				for (const auto& File : Files)
				{
					const auto& R = File.Replacement;
					if (R.Destination.empty() || R.Backup.empty() || R.bPublished || R.bBackedUp
						|| (R.Staged.empty() && !File.Bytes.empty())
						|| !Paths.insert(R.Destination.lexically_normal()).second
						|| !Paths.insert(R.Backup.lexically_normal()).second
						|| (!R.Staged.empty() && !Paths.insert(R.Staged.lexically_normal()).second))
					{
						return Reject(UnifiedError::FError{PackageWriteErrors::InvalidState, "Writer paths must be nonempty and distinct, with inactive replacements."});
					}
				}
				for (const auto& File : Files)
				{
					for (const auto& Path : {File.Replacement.Backup, File.Replacement.Staged})
					{
						if (Path.empty()) continue;
						std::error_code Ec;
						const bool Exists = std::filesystem::exists(Path, Ec);
						if (Ec || Exists) return Reject(ToError(FFileError{.Operation = EFileOperation::Inspect,
							.NativeError = Ec ? Ec : std::make_error_code(std::errc::file_exists), .Path = Path}));
					}
				}
				for (auto& File : Files)
				{
					std::error_code Ec;
					std::filesystem::create_directories(File.Replacement.Destination.parent_path(), Ec);
					std::expected<void, FFileError> Saved;
					if (!Ec && !File.Replacement.Staged.empty())
						Saved = FFileHelper::SaveArrayToNewFile(File.Bytes, File.Replacement.Staged);
					if (Ec || !Saved)
					{
						const auto FileFailure = Ec ? FFileError{.Operation = EFileOperation::CreateParentDirectories,
							.NativeError = Ec, .Path = File.Replacement.Destination.parent_path()} : Saved.error();
						return Reject(ToError(FileFailure));
					}
					if (!File.Replacement.Staged.empty()) OwnedStages.push_back(File.Replacement.Staged);
					FFilePublicationStamp Stamp;
					std::optional<FFileError> InspectionError;
					if (!File.Replacement.Staged.empty()
						&& (!FFilePublicationStamp::Inspect(File.Replacement.Staged, Stamp, &InspectionError)
							|| !Stamp.Exists || Stamp.Size != File.Bytes.size()))
					{
						return Reject(InspectionError ? ToError(*InspectionError)
							: UnifiedError::FError{PackageWriteErrors::CorruptFile, "Cannot verify staged package metadata."});
					}
					StagedStamps.push_back(Stamp);
					// Publication needs only paths and metadata; release bytes on the staging thread.
					FByteBuffer{}.swap(File.Bytes);
				}
				bStaged = true;
				return true;
			}
			auto Commit() -> FPackageWriteResult override
			{
				if (bTerminal || bCommitted) return Result;
				if (!bStaged) return Failure(PackageWriteErrors::InvalidState, "Writer has not staged its output.");
				if (!Access) Access = AcquireWriteFiles(Files);
				if (!Access) return Failure(PackageWriteErrors::InvalidState, "Package output is in use.");
				auto Reject = [&](UnifiedError::FErrorId Error, const char* Message) {
					Access.reset();
					return Failure(Error, Message);
				};
				for (size_t Index = 0; Index < Files.size(); ++Index)
				{
					const auto& File = Files[Index];
					FFilePublicationStamp Current;
					std::optional<FFileError> InspectionError;
					if (!FFilePublicationStamp::Inspect(File.Replacement.Destination, Current, &InspectionError))
					{ Access.reset(); return FPackageWriteResult{.Error = ToError(*InspectionError)}; }
					if (Current != File.Expected)
						return Reject(PackageWriteErrors::StaleData, "Package destination changed while saving.");
					if (!File.Replacement.Staged.empty())
					{
						if (!FFilePublicationStamp::Inspect(File.Replacement.Staged, Current, &InspectionError))
						{ Access.reset(); return FPackageWriteResult{.Error = ToError(*InspectionError)}; }
						if (Current != StagedStamps[Index])
							return Reject(PackageWriteErrors::CorruptFile, "Staged package metadata changed.");
					}
				}
				for (auto& File : Files)
				{
					std::optional<FFileError> Error;
					if (!File.Replacement.Publish(&Error))
					{
						auto Restored = Rollback();
						Result = {.Error = ToError(*Error), .State = Restored.State};
						if (Restored.Error)
						{
							Result.Error->PushContext("rollback domain", Restored.Error->GetDomain())
								.PushContext("rollback code", Restored.Error->GetCode());
							for (const auto& Context : Restored.Error->GetContexts())
								Result.Error->PushContext("rollback " + Context.Key, Context.Value);
						}
						Result.RecoveryFiles = std::move(Restored.RecoveryFiles);
						Result.AffectedFiles = std::move(Restored.AffectedFiles);
						return Result;
					}
					std::erase(OwnedStages, File.Replacement.Staged);
				}
				bCommitted = true;
				return Result = {.State = EPackageWriteState::Committed};
			}
			auto Rollback() -> FPackageWriteResult override
			{
				if (bTerminal) return Result;
				Result = {};
				for (auto It = Files.rbegin(); It != Files.rend(); ++It)
				{
					std::optional<FFileError> Error;
					if (!It->Replacement.Rollback(&Error))
					{
						if (!Result.Error) Result.Error = ToError(*Error);
						else Result.Error->PushContext("additional file failure", Error->ToString());
						Result.State = EPackageWriteState::RecoveryRequired;
					}
				}
				RecoveryFiles(Result);
				bTerminal = true; bCommitted = false;
				Cleanup();
				Access.reset();
				return Result;
			}
			auto Finalize() -> FPackageWriteResult override
			{
				if (bTerminal) return Result;
				if (!bCommitted) return Failure(PackageWriteErrors::InvalidState, "No committed output to finalize.");
				for (auto& File : Files)
				{
					std::optional<FFileError> Error;
					if (!File.Replacement.Finalize(&Error))
					{
						if (!Result.Error) Result.Error = ToError(*Error);
						else Result.Error->PushContext("additional file failure", Error->ToString());
					}
				}
				RecoveryFiles(Result);
				bTerminal = true; bCommitted = false;
				Cleanup();
				Access.reset();
				return Result;
			}
		};
		class FFilePackageWriter final : public IPackageWriter
		{
			auto Begin(std::vector<FPackageWriteFile> Files) -> std::unique_ptr<IPackageWriteOperation> override
			{ return std::make_unique<FFilePackageWriteOperation>(std::move(Files)); }
		};

	}
	auto FPackageFileAccess::TryAcquire(std::span<const std::filesystem::path> InPaths, bool bInWrite)
		-> std::shared_ptr<FPackageFileAccess>
	{
		auto Access = std::shared_ptr<FPackageFileAccess>(new FPackageFileAccess());
		for (const auto& Path : InPaths)
		{
			if (Path.empty()) return {};
			std::error_code Ec;
			const auto Absolute = std::filesystem::absolute(Path, Ec);
			if (Ec) return {};
			auto Normalized = std::filesystem::weakly_canonical(Absolute, Ec);
			if (Ec) return {};
			// Hard-linked outputs cannot be represented by one canonical path key.
			// Reject them rather than admitting aliases under independent locks.
			if (std::filesystem::exists(Normalized, Ec)
				&& std::filesystem::hard_link_count(Normalized, Ec) > 1) return {};
			if (Ec) return {};
#if defined(_WIN32)
			auto Native = Normalized.native();
			std::ranges::transform(Native, Native.begin(), [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); });
			Normalized = Native;
#endif
			if (!std::ranges::contains(Access->Paths, Normalized)) Access->Paths.push_back(std::move(Normalized));
		}
		std::lock_guard Lock(FileAccessMutex);
		for (const auto& Path : Access->Paths)
		{
			const auto It = FileAccessCounts.find(Path);
			if (It != FileAccessCounts.end() && (It->second.bWriter || (bInWrite && It->second.Readers)))
			{
				Access->Paths.clear();
				return {};
			}
		}
		Access->bWrite = bInWrite;
		for (const auto& Path : Access->Paths)
		{
			auto& Count = FileAccessCounts[Path];
			if (bInWrite) Count.bWriter = true; else ++Count.Readers;
		}
		Access->bAcquired = true;
		return Access;
	}
	FPackageFileAccess::~FPackageFileAccess()
	{
		if (!bAcquired || Paths.empty()) return;
		std::lock_guard Lock(FileAccessMutex);
		for (const auto& Path : Paths)
		{
			auto It = FileAccessCounts.find(Path);
			if (It == FileAccessCounts.end()) continue;
			if (bWrite) It->second.bWriter = false; else --It->second.Readers;
			if (!It->second.bWriter && !It->second.Readers) FileAccessCounts.erase(It);
		}
	}
	auto GetFilePackageWriter() -> std::shared_ptr<IPackageWriter>
	{
		static const auto Writer = std::make_shared<FFilePackageWriter>();
		return Writer;
	}
}
