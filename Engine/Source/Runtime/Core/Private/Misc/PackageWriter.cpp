#include "Misc/PackageWriter.h"
#include "Misc/FileHelper.h"
#include <cwctype>

namespace Durin
{
	namespace
	{
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
			auto ReserveCommit() -> FPackageWriteResult override
			{
				if (!Access) Access = AcquireWriteFiles(Files);
				return Access ? FPackageWriteResult{} : FPackageWriteResult{EPackageWriteError::InvalidState, "Package output is in use."};
			}
			~FFilePackageWriteOperation() override
			{
				if (bCommitted && !bTerminal) (void)Rollback();
				Cleanup();
			}
			auto Stage() -> FPackageWriteResult override
			{
				if (bTerminal) return Result;
				if (bStaged || bCommitted) return {EPackageWriteError::InvalidState, "Writer is already staged."};
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
						bTerminal = true;
						return Result = {EPackageWriteError::InvalidState, "Writer paths must be nonempty and distinct, with inactive replacements."};
					}
				}
				for (const auto& File : Files)
				{
					std::error_code Ec;
					const auto& Replacement = File.Replacement;
					if (std::filesystem::exists(Replacement.Backup, Ec) || Ec
						|| (!Replacement.Staged.empty() && (std::filesystem::exists(Replacement.Staged, Ec) || Ec)))
					{
						bTerminal = true;
						return Result = {EPackageWriteError::IoError, "Writer staging or backup path is occupied."};
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
						bTerminal = true;
						Cleanup();
						Result = {EPackageWriteError::IoError};
						Result.FileCause = Ec ? FFileError{.Operation = EFileOperation::CreateParentDirectories,
							.NativeError = Ec, .Path = File.Replacement.Destination.parent_path()} : Saved.error();
						Result.Message = Result.FileCause->ToString();
						return Result;
					}
					if (!File.Replacement.Staged.empty()) OwnedStages.push_back(File.Replacement.Staged);
					FFilePublicationStamp Stamp;
					if (!File.Replacement.Staged.empty()
						&& (!FFilePublicationStamp::Inspect(File.Replacement.Staged, Stamp)
							|| !Stamp.Exists || Stamp.Size != File.Bytes.size()))
					{
						bTerminal = true;
						Cleanup();
						return Result = {EPackageWriteError::IoError, "Cannot verify staged package metadata."};
					}
					StagedStamps.push_back(Stamp);
					// Publication needs only paths and metadata; release bytes on the staging thread.
					FByteBuffer{}.swap(File.Bytes);
				}
				bStaged = true;
				return {};
			}
			auto Commit() -> FPackageWriteResult override
			{
				if (bTerminal || bCommitted) return Result;
				if (!bStaged) return {EPackageWriteError::InvalidState, "Writer has not staged its output."};
				if (!Access) Access = AcquireWriteFiles(Files);
				if (!Access) return {EPackageWriteError::InvalidState, "Package output is in use."};
				auto Reject = [&](EPackageWriteError Error, const char* Message) {
					Access.reset();
					return FPackageWriteResult{Error, Message};
				};
				for (size_t Index = 0; Index < Files.size(); ++Index)
				{
					const auto& File = Files[Index];
					FFilePublicationStamp Current;
					if (!FFilePublicationStamp::Inspect(File.Replacement.Destination, Current) || Current != File.Expected)
						return Reject(EPackageWriteError::StaleData, "Package destination changed while saving.");
					if (!File.Replacement.Staged.empty())
					{
						if (!FFilePublicationStamp::Inspect(File.Replacement.Staged, Current) || Current != StagedStamps[Index])
							return Reject(EPackageWriteError::CorruptFile, "Staged package metadata changed.");
					}
				}
				for (auto& File : Files)
				{
					std::string Error;
					if (!File.Replacement.Publish(Error))
					{
						auto Restored = Rollback();
						Result = {EPackageWriteError::IoError, Error + (Restored ? "" : "; rollback: " + Restored.Message),
							Restored.State, std::move(Restored.RecoveryFiles)};
						return Result;
					}
					std::erase(OwnedStages, File.Replacement.Staged);
				}
				bCommitted = true;
				return Result = {EPackageWriteError::None, {}, EPackageWriteState::Committed};
			}
			auto Rollback() -> FPackageWriteResult override
			{
				if (bTerminal) return Result;
				Result = {};
				for (auto It = Files.rbegin(); It != Files.rend(); ++It)
				{
					std::string Error;
					if (!It->Replacement.Rollback(Error))
					{
						Result.Error = EPackageWriteError::IoError;
						Result.State = EPackageWriteState::RecoveryRequired;
						Result.Message += Error + "; ";
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
				if (!bCommitted) return {EPackageWriteError::InvalidState, "No committed output to finalize."};
				for (auto& File : Files)
				{
					std::string Error;
					if (!File.Replacement.Finalize(Error))
					{ Result.Error = EPackageWriteError::IoError; Result.Message += Error + "; "; }
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
