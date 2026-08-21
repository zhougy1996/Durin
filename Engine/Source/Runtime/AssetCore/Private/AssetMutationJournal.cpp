#include "AssetMutationJournalInternal.h"
#include "AssetPackageVersionPolicy.h"

#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/LexicalPath.h"

namespace Durin::Asset::Private
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}
	}

	FAssetMutationJournal::~FAssetMutationJournal()
	{
		if (State == EAssetMutationState::RecoveryRequired) return;
		const std::string ExpectedOwner = std::format(
			"durin-asset-mutation\\n{}\\n", OperationId);
		for (const std::filesystem::path& Root : Roots)
		{
			if (Root.filename() != std::format("operation-{}", OperationId)
				|| Root.parent_path().filename() != ".durin-asset-mutation")
				continue;
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(Root / "owner", ErrorCode))
				continue;
			std::vector<uint8> OwnerBytes;
			if (!FFileHelper::LoadFileToArray(
					OwnerBytes, (Root / "owner").generic_string())
				|| std::string_view(
					reinterpret_cast<const char*>(OwnerBytes.data()),
					OwnerBytes.size()) != ExpectedOwner)
				continue;
			std::filesystem::remove_all(Root, ErrorCode);
		}
		if (LocatorPath.filename() == std::format("operation-{}", OperationId)
			&& LocatorPath.parent_path().filename() == "AssetMutationRecovery")
		{
			std::error_code ErrorCode;
			std::filesystem::remove(LocatorPath, ErrorCode);
			std::filesystem::remove(LocatorPath.parent_path(), ErrorCode);
		}
	}

	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		std::span<const uint8> Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(EAssetError::IoError, std::format(
				"Failed to read the last-write time for asset package {}.", PhysicalPath));
		uint32 Magic = 0;
		uint32 Version = 0;
		if (Bytes.size() >= sizeof(Magic) + sizeof(Version))
		{
			std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
			std::memcpy(&Version, Bytes.data() + sizeof(Magic), sizeof(Version));
		}
		OutFingerprint = {
			.FileSize = Bytes.size(),
			.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime),
			.ContentHash = FXxHash128::HashBuffer(Bytes),
			.ReaderVersion = Magic == DastPackageMagic ? Version : 0};
		return {};
	}


	auto MakeRelocationOperationId() -> std::string
	{
		static std::atomic<uint64> Counter = 1;
		const uint64 Time = static_cast<uint64>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		return std::format("{:016x}{:016x}", Time, Counter++);
	}

	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path
	{
		return std::filesystem::absolute(Path).lexically_normal();
	}

	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		std::vector<uint8>& OutBytes) -> FAssetResult
	{
		OutBytes.clear();
		if (!FFileHelper::LoadFileToArray(OutBytes, Path.generic_string()))
			return Error(EAssetError::IoError, std::format(
				"Could not read relocation input {}.", Path.generic_string()));
		return {};
	}

	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		std::span<const uint8> Bytes) -> FAssetResult
	{
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()),
					Bytes.size()},
				Path,
				&PublicationError))
			return Error(EAssetError::IoError, PublicationError.ToString());
		return {};
	}

	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
	{
		std::vector<uint8> Bytes;
		FAssetResult Result = LoadRelocationBytes(Path, Bytes);
		if (!Result) return Result;
		return MakePackageFingerprint(
			Path.generic_string(), Bytes, OutFingerprint);
	}

	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const PathUtilities::FMountPoint*& OutMount,
		std::string& OutError) -> bool
	{
		OutMount = nullptr;
		const std::filesystem::path Normalized = NormalizePhysicalPath(Path);
		for (const PathUtilities::FMountPoint& Mount :
			PathUtilities::GetRegisteredMountPoints())
		{
			const std::filesystem::path Content =
				NormalizePhysicalPath(Mount.GetContentDir());
			if (!PathUtilities::IsLexicalDescendantPath(
					Normalized.generic_string(), Content.generic_string(), true))
				continue;
			if (!Mount.bAuthoringWritable)
			{
				OutError = std::format(
					"Content mount {} is read-only.", Mount.VirtualRoot);
				return false;
			}
			for (std::filesystem::path Current = Normalized.parent_path();
				!Current.empty(); Current = Current.parent_path())
			{
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(
					Current, StatusError);
				if (!StatusError && std::filesystem::is_symlink(Status))
				{
					OutError = std::format(
						"Relocation path traverses a reparse point: {}.",
						Current.generic_string());
					return false;
				}
				if (Current == Content) break;
				if (Current == Current.root_path()) break;
			}
			OutMount = &Mount;
			return true;
		}
		OutError = std::format(
			"Relocation path is outside writable mounted content: {}.",
			Path.generic_string());
		return false;
	}


	auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> void
	{
		std::string Text = std::format(
			"version=1\noperation={}\ntype={}\nstate={}\nentries={}\n",
			Journal.OperationId,
			Journal.OperationType,
			static_cast<uint32>(Journal.State),
			Journal.Entries.size());
		for (size_t Index = 0; Index < Journal.Entries.size(); ++Index)
		{
			const FAssetMutationJournalEntry& Entry = Journal.Entries[Index];
			Text += std::format(
				"entry.{}.role={}\n"
				"entry.{}.order={}\n"
				"entry.{}.registry={}\n"
				"entry.{}.original={}\n"
				"entry.{}.staged_pre={}\n"
				"entry.{}.staged_post={}\n"
				"entry.{}.published={}\n"
				"entry.{}.pre_exists={}\n"
				"entry.{}.post_exists={}\n"
				"entry.{}.pre_fingerprint={}:{}:{}\n"
				"entry.{}.post_fingerprint={}:{}:{}\n"
				"entry.{}.staged_pre_hash={}\n"
				"entry.{}.staged_post_hash={}\n"
				"entry.{}.completed={}\n"
				"entry.{}.compensated={}\n",
				Index, static_cast<uint32>(Entry.Role),
				Index, Entry.PublicationOrder,
				Index, Entry.RegistryPath.ToString(),
				Index, Entry.PhysicalPath.generic_string(),
				Index, Entry.StagedPrePath.generic_string(),
				Index, Entry.StagedPostPath.generic_string(),
				Index, Entry.PhysicalPath.generic_string(),
				Index, Entry.bPreExists,
				Index, Entry.bPostExists,
				Index, Entry.ExpectedPreFingerprint.FileSize,
				Entry.ExpectedPreFingerprint.LastWriteTimeTicks,
				Entry.ExpectedPreFingerprint.ContentHash.ToString(),
				Index, Entry.ExpectedPostFingerprint.FileSize,
				Entry.ExpectedPostFingerprint.LastWriteTimeTicks,
				Entry.ExpectedPostFingerprint.ContentHash.ToString(),
				Index, Entry.StagedPreHash.ToString(),
				Index, Entry.StagedPostHash.ToString(),
				Index, Entry.bCompleted,
				Index, Entry.bCompensated);
		}
		const std::span Bytes{
			reinterpret_cast<const uint8*>(Text.data()), Text.size()};
		for (const std::filesystem::path& Root : Journal.Roots)
		{
			std::string Ignored;
			DerivedDataCache::WriteFileAtomically(
				Root / "journal", Bytes, &Ignored);
		}

		std::string Locator = std::format(
			"version=1\noperation={}\nroots={}\n",
			Journal.OperationId, Journal.Roots.size());
		for (const std::filesystem::path& Root : Journal.Roots)
			Locator += std::format("root={}\n", Root.generic_string());
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Journal.LocatorPath.parent_path(), DirectoryError);
		if (!DirectoryError)
		{
			const std::span LocatorBytes{
				reinterpret_cast<const uint8*>(Locator.data()), Locator.size()};
			std::string Ignored;
			DerivedDataCache::WriteFileAtomically(
				Journal.LocatorPath, LocatorBytes, &Ignored);
		}
	}


	auto PublishRelocationFile(
		const FAssetMutationJournalEntry& Entry,
		bool bForward) -> FAssetResult
	{
		const bool bExists = bForward
			? Entry.bPostExists : Entry.bPreExists;
		const std::filesystem::path& Staged = bForward
			? Entry.StagedPostPath : Entry.StagedPrePath;
		if (!bExists)
		{
			std::error_code RemoveError;
			if (!std::filesystem::remove(Entry.PhysicalPath, RemoveError)
				&& RemoveError)
				return Error(EAssetError::IoError, std::format(
					"Could not remove relocation input {}: {}",
					Entry.PhysicalPath.generic_string(),
					RemoveError.message()));
			return {};
		}
		std::vector<uint8> Bytes;
		FAssetResult Result = LoadRelocationBytes(Staged, Bytes);
		if (!Result) return Result;
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Entry.PhysicalPath.parent_path(), DirectoryError);
		if (DirectoryError)
			return Error(EAssetError::IoError, std::format(
				"Could not create relocation destination directory: {}",
				DirectoryError.message()));
		return SaveRelocationBytes(Entry.PhysicalPath, Bytes);
	}

}
