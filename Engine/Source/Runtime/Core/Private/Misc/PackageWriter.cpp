#include "Misc/PackageWriter.h"
#include "Misc/FileIO.h"
#include <cwctype>

namespace Durin
{
	namespace
	{
		struct FFileAccessCount { uint32 Readers = 0; bool bWriter = false; };
		std::mutex FileAccessMutex;
		std::mutex DirectHookMutex;
		std::function<bool(size_t)> DirectFailureHook;
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
					std::expected<void, FFileIO::FFileError> Saved;
					if (!Ec && !File.Replacement.Staged.empty())
						Saved = FFileIO::SaveArrayToNewFile(File.Bytes, File.Replacement.Staged);
					if (Ec || !Saved)
					{
						bTerminal = true;
						Cleanup();
						return Result = {EPackageWriteError::IoError, Ec ? Ec.message() : Saved.error().ToString()};
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
		class FDirectPackageWriteOperation final : public IPackageWriteOperation
		{
			std::vector<FPackageWriteFile> Files;
			std::function<bool(size_t)> ShouldFail;
			FPackageWriteResult Result;
			bool bStarted = false;
			std::shared_ptr<FPackageFileAccess> Access;
		public:
			explicit FDirectPackageWriteOperation(std::vector<FPackageWriteFile> InFiles, std::function<bool(size_t)> InShouldFail)
				: Files(std::move(InFiles)), ShouldFail(std::move(InShouldFail)), Access(AcquireWriteFiles(Files)) {}
			auto GetAdmissionResult() const -> FPackageWriteResult override
			{
				return Access && Access->GetPathCount() == Files.size() ? FPackageWriteResult{}
					: FPackageWriteResult{EPackageWriteError::InvalidState, "Package output is in use or contains aliases."};
			}
			auto Stage() -> FPackageWriteResult override
			{
				if (bStarted) return Result;
				bStarted = true;
				if (!Access || Access->GetPathCount() != Files.size())
					return Result = {EPackageWriteError::InvalidState, "Package output is in use or contains aliases."};
				std::unordered_set<std::filesystem::path> Destinations;
				// Check the complete input before the first destructive operation.
				for (const auto& File : Files)
				{
					const auto& R = File.Replacement;
					if (R.Destination.empty() || R.bPublished || R.bBackedUp
						|| (R.Staged.empty() && !File.Bytes.empty())
						|| File.Bytes.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())
						|| !Destinations.insert(R.Destination.lexically_normal()).second)
						return Result = {EPackageWriteError::InvalidState, "Direct writer destinations must be distinct and inactive."};
					FFilePublicationStamp Current;
					if (!FFilePublicationStamp::Inspect(R.Destination, Current) || Current != File.Expected)
						return Result = {EPackageWriteError::StaleData, "Direct writer destination changed before writing."};
				}
				size_t Index = 0;
				for (auto& File : Files)
				{
					const auto& R = File.Replacement;
					if (ShouldFail && ShouldFail(Index++))
					{
						Result.Error = EPackageWriteError::IoError;
						Result.Message = "Injected direct output failure: " + R.Destination.string();
						return Result;
					}
					std::error_code Ec;
					std::filesystem::create_directories(R.Destination.parent_path(), Ec);
					if (Ec)
					{
						Result.Error = EPackageWriteError::IoError;
						Result.Message = "Cannot create direct output directory: " + R.Destination.string() + ": " + Ec.message();
						return Result;
					}
					// A failed open/write/close may already have truncated the file.
					Result.State = EPackageWriteState::PartiallyWritten;
					Result.AffectedFiles.push_back(R.Destination);
					bool Written = false;
					if (R.Staged.empty())
					{
						std::filesystem::remove(R.Destination, Ec);
						Written = !Ec;
					}
					else
					{
						std::ofstream Stream(R.Destination, std::ios::binary | std::ios::trunc);
						if (Stream)
						{
							Stream.write(reinterpret_cast<const char*>(File.Bytes.data()),
								static_cast<std::streamsize>(File.Bytes.size()));
							Stream.flush();
							Stream.close();
							Written = !Stream.fail();
						}
					}
					if (!Written)
					{
						Result.Error = EPackageWriteError::IoError;
						Result.Message = "Direct output failed; old content was not restored: " + R.Destination.string();
						return Result;
					}
					FByteBuffer{}.swap(File.Bytes);
				}
				Result.State = EPackageWriteState::Committed;
				Result.AffectedFiles.clear();
				return Result;
			}
			auto Commit() -> FPackageWriteResult override
			{
				return bStarted ? Result : FPackageWriteResult{EPackageWriteError::InvalidState, "Direct output has not run."};
			}
			auto Finalize() -> FPackageWriteResult override { return Commit(); }
			auto Rollback() -> FPackageWriteResult override
			{
				if (!bStarted || Result.State == EPackageWriteState::NotCommitted) return Result;
				auto Failure = Result;
				Failure.Error = EPackageWriteError::InvalidState;
				Failure.Message = "Direct output cannot restore old content.";
				return Failure;
			}
		};
		class FDirectPackageWriter final : public IPackageWriter
		{
			std::function<bool(size_t)> ShouldFail;
		public:
			explicit FDirectPackageWriter(std::function<bool(size_t)> InShouldFail) : ShouldFail(std::move(InShouldFail)) {}
			auto SupportsRollback() const -> bool override { return false; }
			auto Begin(std::vector<FPackageWriteFile> Files) -> std::unique_ptr<IPackageWriteOperation> override
			{ return std::make_unique<FDirectPackageWriteOperation>(std::move(Files), ShouldFail); }
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
			if (std::ranges::find(Access->Paths, Normalized) == Access->Paths.end()) Access->Paths.push_back(std::move(Normalized));
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
	auto GetDirectFilePackageWriter(std::function<bool(size_t)> ShouldFail) -> std::shared_ptr<IPackageWriter>
	{
		if (!ShouldFail) { std::lock_guard Lock(DirectHookMutex); ShouldFail = DirectFailureHook; }
		return std::make_shared<FDirectPackageWriter>(std::move(ShouldFail));
	}
	namespace Private
	{
		auto SetDirectPackageWriteFailureForTests(std::function<bool(size_t)> Hook) -> void
		{ std::lock_guard Lock(DirectHookMutex); DirectFailureHook = std::move(Hook); }
	}
}
