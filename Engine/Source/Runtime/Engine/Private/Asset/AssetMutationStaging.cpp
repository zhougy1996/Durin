#include "Asset/AssetWriteResult.h"
#include "AssetMutationStagingInternal.h"
#include "AssetPackageCodec.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"


namespace Durin::AssetPrivate
{
	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}

		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult
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

		auto MakeMutationStagingOwnerMarker(std::string_view OperationId)
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

	FAssetMutationStaging::~FAssetMutationStaging()
	{
		if (bRetainBackups) return;
		const std::string ExpectedOwner =
			MakeMutationStagingOwnerMarker(OperationId);
		for (const std::filesystem::path& Root : Roots)
		{
			if (Root.filename() != std::format("operation-{}", OperationId)
				|| Root.parent_path().filename() != ".durin-asset-mutation")
				continue;
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(Root / "owner", ErrorCode))
				continue;
			FByteBuffer OwnerBytes;
			if (!FFileHelper::LoadFileToArray(
					OwnerBytes, (Root / "owner"))
				|| std::string_view(
					reinterpret_cast<const char*>(OwnerBytes.data()),
					OwnerBytes.size()) != ExpectedOwner)
				continue;
			std::filesystem::remove_all(Root, ErrorCode);
		}
	}

	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(EAssetReadError::IoError, std::format(
				"Failed to read the last-write time for asset package {}.", PhysicalPath));
		uint32 ReaderVersion = 0;
		if (Path.extension() == ".dasset")
		{
			const FAssetPackageCodec* Codec = nullptr;
			const auto ResolveResult = ResolveAssetPackageReader(
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

	auto InitializeMutationStaging(FAssetMutationStaging& Staging) -> void
	{
		Staging.OperationId = MakeMutationOperationId();
	}

	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path
	{
		return std::filesystem::absolute(Path).lexically_normal();
	}

	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteBuffer& OutBytes) -> FAssetReadResult
	{
		OutBytes.clear();
		if (!FFileHelper::LoadFileToArray(OutBytes, Path))
			return Error(EAssetReadError::IoError, std::format(
				"Could not read relocation input {}.", Path.generic_string()));
		return {};
	}

	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		FByteView Bytes) -> FAssetWriteResult
	{
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()),
					Bytes.size()},
				Path,
				&PublicationError))
			return Error(EAssetWriteError::IoError, PublicationError.ToString());
		return {};
	}

	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult
	{
		FByteBuffer Bytes;
		auto Result = LoadRelocationBytes(Path, Bytes);
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

	auto StageMutationEntry(
		FAssetMutationStaging& Staging,
		const FMutationStageRequest& Request,
		size_t& OutEntryIndex) -> FAssetWriteResult
	{
		OutEntryIndex = std::numeric_limits<size_t>::max();
		const std::filesystem::path Normalized =
			NormalizePhysicalPath(Request.PhysicalPath);
		const std::string Key = Normalized.generic_string();
		FAssetMutationStagingEntry Entry{
			.PhysicalPath = Normalized,
			.RegistryPath = Request.RegistryPath,
			.Role = Request.Role,
			.bPreExists = Request.bPreExists,
			.bPostExists = Request.bPostExists};
		if (Request.bPostExists)
		{
			Entry.StagedPostHash = FXxHash128::HashBuffer(Request.PostBytes);
		}

		std::string PathError;
		const FMountPoint* Mount = nullptr;
		if (!IsWritableRelocationPath(Normalized, Mount, PathError))
			return Error(EAssetWriteError::ReadOnlyMode, std::move(PathError));
		if (Request.bPreExists && IsReadOnlyMutationInput(Normalized))
			return Error(EAssetWriteError::ReadOnlyMode, std::format(
				"Asset mutation input is read-only: {}.", Key));
		if (Request.bPreExists)
		{
			FAssetWriteResult Result = AssetWriteResultFromRead(MakePackageFingerprint(
				Key, Request.PreBytes, Entry.ExpectedPreFingerprint));
			if (!Result) return Result;
		}

		if (const auto Existing = Staging.EntryIndices.find(Key);
			Existing != Staging.EntryIndices.end())
		{
			const FAssetMutationStagingEntry& ExistingEntry =
				Staging.Entries[Existing->second];
			const bool bEquivalent =
				ExistingEntry.RegistryPath == Entry.RegistryPath
				&& ExistingEntry.Role == Entry.Role
				&& ExistingEntry.bPreExists == Entry.bPreExists
				&& ExistingEntry.bPostExists == Entry.bPostExists
				&& ExistingEntry.ExpectedPreFingerprint.ContentHash == Entry.ExpectedPreFingerprint.ContentHash
				&& ExistingEntry.StagedPostHash == Entry.StagedPostHash;
			if (Request.DuplicatePolicy
					== EMutationStagingDuplicatePolicy::ReuseEquivalent
				&& bEquivalent)
			{
				OutEntryIndex = Existing->second;
				return {};
			}
			return Error(EAssetWriteError::AlreadyExists, std::format(
				"Asset mutation participants claim the same file {}.", Key));
		}

		const size_t Index = Staging.Entries.size();
		const std::filesystem::path Root =
			NormalizePhysicalPath(Mount->GetContentDir())
			/ ".durin-asset-mutation"
			/ std::format("operation-{}", Staging.OperationId);
		bool bCreatedRoot = false;
		if (std::ranges::find(Staging.Roots, Root) == Staging.Roots.end())
		{
			std::error_code DirectoryError;
			bCreatedRoot = std::filesystem::create_directories(
				Root, DirectoryError);
			if (DirectoryError)
				return Error(EAssetWriteError::IoError, std::format(
					"Could not create asset mutation staging root: {}",
					DirectoryError.message()));
			if (!bCreatedRoot)
				return Error(EAssetWriteError::AlreadyExists, std::format(
					"Asset mutation staging root already exists: {}.",
					Root.generic_string()));
			const std::string Marker =
				MakeMutationStagingOwnerMarker(Staging.OperationId);
			FAssetWriteResult MarkerResult = SaveRelocationBytes(
				Root / "owner", std::as_bytes(std::span(Marker)));
			if (!MarkerResult)
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Root, CleanupError);
				return MarkerResult;
			}
			Staging.Roots.push_back(Root);
		}

		const auto BackupPath = Root / std::format("pre-{:08}", Index);
		Entry.StagedPostPath = Root / std::format("post-{:08}", Index);
		auto CleanupStagedEntry = [&] {
			std::error_code CleanupError;
			std::filesystem::remove(BackupPath, CleanupError);
			CleanupError.clear();
			std::filesystem::remove(Entry.StagedPostPath, CleanupError);
			if (!bCreatedRoot) return;
			CleanupError.clear();
			std::filesystem::remove_all(Root, CleanupError);
			if (!CleanupError) std::erase(Staging.Roots, Root);
		};
		if (Request.bPreExists)
		{
			FAssetWriteResult Result = SaveRelocationBytes(
				BackupPath, Request.PreBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		if (Request.bPostExists)
		{
			FAssetWriteResult Result = SaveRelocationBytes(
				Entry.StagedPostPath, Request.PostBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		Staging.Entries.push_back(std::move(Entry));
		Staging.EntryIndices.emplace(Key, Index);
		OutEntryIndex = Index;
		return {};
	}

	auto PublishRelocationFile(
		const FAssetMutationStagingEntry& Entry) -> FAssetWriteResult
	{
		if (!Entry.bPostExists)
		{
			std::error_code RemoveError;
			if (!std::filesystem::remove(Entry.PhysicalPath, RemoveError)
				&& RemoveError)
				return Error(EAssetWriteError::IoError, std::format(
					"Could not remove relocation input {}: {}",
					Entry.PhysicalPath.generic_string(),
					RemoveError.message()));
			return {};
		}
		FByteBuffer Bytes;
		FAssetWriteResult Result = AssetWriteResultFromRead(LoadRelocationBytes(Entry.StagedPostPath, Bytes));
		if (!Result) return Result;
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Entry.PhysicalPath.parent_path(), DirectoryError);
		if (DirectoryError)
			return Error(EAssetWriteError::IoError, std::format(
				"Could not create relocation destination directory: {}",
				DirectoryError.message()));
		return SaveRelocationBytes(Entry.PhysicalPath, Bytes);
	}

}
