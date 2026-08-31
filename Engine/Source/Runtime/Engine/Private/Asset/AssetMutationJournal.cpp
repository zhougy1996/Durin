#include "AssetMutationJournalInternal.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::Asset::Private
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto MakeMutationOperationId() -> std::string
		{
			static std::atomic<uint64> Counter = 1;
			const uint64 Time = static_cast<uint64>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			return std::format("{:016x}{:016x}", Time, Counter++);
		}

		auto GetMutationOperationType(
			EAssetMutationOperationKind OperationKind) -> std::string_view
		{
			switch (OperationKind)
			{
			case EAssetMutationOperationKind::Relocation:
				return "relocation";
			case EAssetMutationOperationKind::RedirectorFixup:
				return "fixup";
			case EAssetMutationOperationKind::Deletion:
				return "deletion";
			}
			return "unknown";
		}

		auto MakeMutationJournalOwnerMarker(std::string_view OperationId)
			-> std::string
		{
			return std::format("durin-asset-mutation\n{}\n", OperationId);
		}

		auto IsReadOnlyMutationInput(const std::filesystem::path& Path) -> bool
		{
			std::error_code ErrorCode;
			const std::filesystem::perms Permissions =
				std::filesystem::status(Path, ErrorCode).permissions();
			constexpr auto WritePermissions =
				std::filesystem::perms::owner_write
				| std::filesystem::perms::group_write
				| std::filesystem::perms::others_write;
			return ErrorCode || (Permissions & WritePermissions)
				== std::filesystem::perms::none;
		}
	}

	FAssetMutationJournal::~FAssetMutationJournal()
	{
		if (IsMutationJournalRecoveryRequired(*this)) return;
		const std::string ExpectedOwner =
			MakeMutationJournalOwnerMarker(OperationId);
		for (const std::filesystem::path& Root : Roots)
		{
			if (Root.filename() != std::format("operation-{}", OperationId)
				|| Root.parent_path().filename() != ".durin-asset-mutation")
				continue;
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(Root / "owner", ErrorCode))
				continue;
			FByteArray OwnerBytes;
			if (!FFileHelper::LoadFileToArray(
					OwnerBytes, (Root / "owner"))
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
		std::span<const std::byte> Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(EAssetError::IoError, std::format(
				"Failed to read the last-write time for asset package {}.", PhysicalPath));
		uint32 ReaderVersion = 0;
		if (Path.extension() == ".dasset")
		{
			const FAssetPackageCodec* Codec = nullptr;
			const FAssetResult ResolveResult = ResolveAssetPackageReader(
				Bytes, Codec, &ReaderVersion);
			if (!ResolveResult) return ResolveResult;
		}
		OutFingerprint = {
			.FileSize = Bytes.size(),
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime),
			.ContentHash = FXxHash128::HashBuffer(Bytes),
			.ReaderVersion = ReaderVersion};
		return {};
	}


	auto InitializeMutationJournal(
		FAssetMutationJournal& Journal,
		EAssetMutationOperationKind OperationKind) -> void
	{
		Journal.OperationId = MakeMutationOperationId();
		Journal.OperationType = GetMutationOperationType(OperationKind);
		const std::string RecoveryBase = FPaths::ProjectDir().empty()
			? FPaths::LaunchDir() : FPaths::ProjectDir();
		Journal.LocatorPath = NormalizePhysicalPath(RecoveryBase)
			/ "Saved" / "AssetMutationRecovery"
			/ std::format("operation-{}", Journal.OperationId);
	}

	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path
	{
		return std::filesystem::absolute(Path).lexically_normal();
	}

	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteArray& OutBytes) -> FAssetResult
	{
		OutBytes.clear();
		if (!FFileHelper::LoadFileToArray(OutBytes, Path))
			return Error(EAssetError::IoError, std::format(
				"Could not read relocation input {}.", Path.generic_string()));
		return {};
	}

	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		std::span<const std::byte> Bytes) -> FAssetResult
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
		FByteArray Bytes;
		FAssetResult Result = LoadRelocationBytes(Path, Bytes);
		if (!Result) return Result;
		return MakePackageFingerprint(
			Path.generic_string(), Bytes, OutFingerprint);
	}

	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const FMountPoint*& OutMount,
		std::string& OutError) -> bool
	{
		OutMount = nullptr;
		const std::filesystem::path Normalized = NormalizePhysicalPath(Path);
		for (const FMountPoint& Mount :
			FMountPaths::GetRegisteredMountPoints())
		{
			const std::filesystem::path Content =
				NormalizePhysicalPath(Mount.GetContentDir());
			if (!FPaths::IsLexicalDescendantPath(
					Normalized.generic_string(), Content.generic_string(), true))
				continue;
			if (!Mount.bContentWritable)
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

	auto StageMutationJournalEntry(
		FAssetMutationJournal& Journal,
		const FMutationJournalStageRequest& Request,
		size_t& OutEntryIndex) -> FAssetResult
	{
		OutEntryIndex = std::numeric_limits<size_t>::max();
		const std::filesystem::path Normalized =
			NormalizePhysicalPath(Request.PhysicalPath);
		const std::string Key = Normalized.generic_string();
		FAssetMutationJournalEntry Entry{
			.PhysicalPath = Normalized,
			.RegistryPath = Request.RegistryPath,
			.Role = Request.Role,
			.bPreExists = Request.bPreExists,
			.bPostExists = Request.bPostExists};
		if (Request.bPreExists)
			Entry.StagedPreHash = FXxHash128::HashBuffer(Request.PreBytes);
		if (Request.bPostExists)
		{
			Entry.StagedPostHash = FXxHash128::HashBuffer(Request.PostBytes);
			Entry.ExpectedPostFingerprint.FileSize = Request.PostBytes.size();
			Entry.ExpectedPostFingerprint.ContentHash = Entry.StagedPostHash;
		}

		if (const auto Existing = Journal.EntryIndices.find(Key);
			Existing != Journal.EntryIndices.end())
		{
			const FAssetMutationJournalEntry& ExistingEntry =
				Journal.Entries[Existing->second];
			const bool bEquivalent =
				ExistingEntry.RegistryPath == Entry.RegistryPath
				&& ExistingEntry.Role == Entry.Role
				&& ExistingEntry.bPreExists == Entry.bPreExists
				&& ExistingEntry.bPostExists == Entry.bPostExists
				&& ExistingEntry.StagedPreHash == Entry.StagedPreHash
				&& ExistingEntry.StagedPostHash == Entry.StagedPostHash;
			if (Request.DuplicatePolicy
					== EMutationJournalDuplicatePolicy::ReuseEquivalent
				&& bEquivalent)
			{
				OutEntryIndex = Existing->second;
				return {};
			}
			return Error(EAssetError::AlreadyExists, std::format(
				"Asset mutation participants claim the same file {}.", Key));
		}

		std::string PathError;
		const FMountPoint* Mount = nullptr;
		if (!IsWritableRelocationPath(Normalized, Mount, PathError))
			return Error(EAssetError::ReadOnlyMode, std::move(PathError));
		if (Request.bPreExists && IsReadOnlyMutationInput(Normalized))
			return Error(EAssetError::ReadOnlyMode, std::format(
				"Asset mutation input is read-only: {}.", Key));
		if (Request.bPreExists)
		{
			FAssetResult Result = MakePackageFingerprint(
				Key, Request.PreBytes, Entry.ExpectedPreFingerprint);
			if (!Result) return Result;
		}

		const size_t Index = Journal.Entries.size();
		const std::filesystem::path Root =
			NormalizePhysicalPath(Mount->GetContentDir())
			/ ".durin-asset-mutation"
			/ std::format("operation-{}", Journal.OperationId);
		bool bCreatedRoot = false;
		if (std::ranges::find(Journal.Roots, Root) == Journal.Roots.end())
		{
			std::error_code DirectoryError;
			bCreatedRoot = std::filesystem::create_directories(
				Root, DirectoryError);
			if (DirectoryError)
				return Error(EAssetError::IoError, std::format(
					"Could not create asset mutation staging root: {}",
					DirectoryError.message()));
			if (!bCreatedRoot)
				return Error(EAssetError::AlreadyExists, std::format(
					"Asset mutation staging root already exists: {}.",
					Root.generic_string()));
			const std::string Marker =
				MakeMutationJournalOwnerMarker(Journal.OperationId);
			FAssetResult MarkerResult = SaveRelocationBytes(
				Root / "owner", std::as_bytes(std::span(Marker)));
			if (!MarkerResult)
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Root, CleanupError);
				return MarkerResult;
			}
			Journal.Roots.push_back(Root);
		}

		Entry.StagedPrePath = Root / std::format("pre-{:08}", Index);
		Entry.StagedPostPath = Root / std::format("post-{:08}", Index);
		auto CleanupStagedEntry = [&] {
			std::error_code CleanupError;
			std::filesystem::remove(Entry.StagedPrePath, CleanupError);
			CleanupError.clear();
			std::filesystem::remove(Entry.StagedPostPath, CleanupError);
			if (!bCreatedRoot) return;
			CleanupError.clear();
			std::filesystem::remove_all(Root, CleanupError);
			if (!CleanupError) std::erase(Journal.Roots, Root);
		};
		if (Request.bPreExists)
		{
			FAssetResult Result = SaveRelocationBytes(
				Entry.StagedPrePath, Request.PreBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		if (Request.bPostExists)
		{
			FAssetResult Result = SaveRelocationBytes(
				Entry.StagedPostPath, Request.PostBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		Journal.Entries.push_back(std::move(Entry));
		Journal.EntryIndices.emplace(Key, Index);
		OutEntryIndex = Index;
		return {};
	}


	auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> FAssetResult
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
		const auto Bytes = std::as_bytes(std::span(Text));
		for (const std::filesystem::path& Root : Journal.Roots)
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
					Bytes, Root / "journal", &PublicationError))
				return Error(EAssetError::IoError, std::format(
					"Could not persist asset mutation journal: {}",
					PublicationError.ToString()));
		}

		std::string Locator = std::format(
			"version=1\noperation={}\nroots={}\n",
			Journal.OperationId, Journal.Roots.size());
		for (const std::filesystem::path& Root : Journal.Roots)
			Locator += std::format("root={}\n", Root.generic_string());
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Journal.LocatorPath.parent_path(), DirectoryError);
		if (DirectoryError)
			return Error(EAssetError::IoError, std::format(
				"Could not create asset mutation recovery locator directory {}: {}",
				Journal.LocatorPath.parent_path().generic_string(),
				DirectoryError.message()));
		const auto LocatorBytes = std::as_bytes(std::span(Locator));
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				LocatorBytes, Journal.LocatorPath, &PublicationError))
			return Error(EAssetError::IoError, std::format(
				"Could not persist asset mutation recovery locator: {}",
				PublicationError.ToString()));
		return {};
	}

	auto TransitionMutationJournalState(
		FAssetMutationJournal& Journal,
		EAssetMutationState State) -> FAssetResult
	{
		const EAssetMutationState PreviousState = Journal.State;
		Journal.State = State;
		FAssetResult Result = WriteMutationJournalState(Journal);
		if (!Result) Journal.State = PreviousState;
		return Result;
	}

	auto IsMutationJournalRecoveryRequired(
		const FAssetMutationJournal& Journal) -> bool
	{
		return Journal.State == EAssetMutationState::Publishing
			|| Journal.State == EAssetMutationState::Compensating
			|| Journal.State == EAssetMutationState::RecoveryRequired;
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
		FByteArray Bytes;
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
