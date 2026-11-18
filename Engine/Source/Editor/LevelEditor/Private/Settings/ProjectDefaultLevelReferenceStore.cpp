#include "Settings/ProjectDefaultLevelReferenceStore.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "Hash/XxHash.h"
#include "DObject/Class.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFile.h"
#include "Project/Project.h"

namespace Durin::LevelEditor
{
	namespace
	{
		constexpr std::string_view ProviderId =
			"Durin.LevelEditor.ProjectDefaultLevel";
		constexpr std::string_view StableId = "Game.DefaultLevel";
		constexpr uint64 ProviderVersion = 2;

		struct FCapturedProjectDefaultLevel
		{
			std::filesystem::path SettingsFile;
			FByteBuffer Bytes;
			std::string Fingerprint;
			FPackagePath Path;
			bool bFileExists = false;
		};

		auto StoreError(Durin::UnifiedError::FErrorId Id, std::string Message)
			-> std::expected<void, Durin::UnifiedError::FError>
		{
			return std::unexpected(Durin::UnifiedError::FError{Id, std::move(Message)});
		}

		auto MakeFingerprint(
			const std::filesystem::path& SettingsFile,
			bool bFileExists,
			FByteView Bytes) -> std::string
		{
			std::string Source = std::format(
				"{}\n{}\n{}\n", ProviderId,
				SettingsFile.lexically_normal().generic_string(), bFileExists);
			Source.append(
				reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			return FXxHash128::HashBuffer(std::as_bytes(std::span{Source}))
				.ToString();
		}

		auto CaptureProjectDefaultLevel(
			const FProjectDefaultLevelReferenceStore::FProjectResolver&
				ProjectResolver,
			FCapturedProjectDefaultLevel& OutState) -> std::expected<void, Durin::UnifiedError::FError>
		{
			OutState = {};
			const FProjectInfo* Project = ProjectResolver
				? ProjectResolver() : nullptr;
			if (!Project)
			{
				OutState.Fingerprint = MakeFingerprint({}, false, {});
				return {};
			}
			OutState.SettingsFile = std::filesystem::path(Project->ProjectDir)
				/ "Configs" / "Project.yaml";
			std::error_code ExistsError;
			OutState.bFileExists = std::filesystem::exists(
				OutState.SettingsFile, ExistsError);
			if (ExistsError)
				return std::unexpected(ToError(FFileSystemError{std::format("Failed to inspect: {}.", FFilePath(OutState.SettingsFile).generic_string()), ExistsError}));
			if (!OutState.bFileExists)
			{
				OutState.Fingerprint = MakeFingerprint(
					OutState.SettingsFile, false, {});
				return {};
			}
			auto Bytes = FFileHelper::LoadFileToArray(OutState.SettingsFile);
			if (!Bytes)
				return std::unexpected(ToError(Bytes.error()));
			OutState.Bytes = std::move(*Bytes);
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult =
				FProjectGameSettingsStore(OutState.SettingsFile).Load(Settings);
			if (!SettingsResult)
				return StoreError(Durin::PackageReadErrors::CorruptFile, SettingsResult.Message);
			if (!Settings.DefaultLevel.empty())
			{
				std::string PathError;
				if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, OutState.Path); !PathValidation)
				{
					PathError = ToString(PathValidation.error());
					return StoreError(Durin::AssetLoadErrors::InvalidPath, std::format("Project default level is invalid: {}",
							PathError));
				}
			}
			OutState.Fingerprint = MakeFingerprint(
				OutState.SettingsFile, true, OutState.Bytes);
			return {};
		}

		auto SaveSettingsBytes(const std::filesystem::path& SettingsFile,
			FByteView Bytes, std::optional<UnifiedError::FError>* OutError) -> bool
		{
			if (OutError) OutError->reset();
			if (Bytes.empty())
			{ if (OutError) OutError->emplace(AssetWriteErrors::IoError, "Project settings serialized to empty bytes."); return false; }
			if (auto Saved = FFileHelper::SaveArrayToFileAtomically(Bytes, SettingsFile); !Saved)
			{ if (OutError) *OutError = ToError(Saved.error()); return false; }
			return true;
		}

	}

	FProjectDefaultLevelReferenceStore::FProjectDefaultLevelReferenceStore(
		FPathChanged InPathChanged,
		FProjectResolver InProjectResolver)
		: PathChanged(std::move(InPathChanged))
		, ProjectResolver(std::move(InProjectResolver))
	{
		if (!ProjectResolver)
			ProjectResolver = [] { return GetCurrentProject(); };
	}

	auto FProjectDefaultLevelReferenceStore::CaptureSnapshot(
		FAssetReferenceStoreSnapshot& OutSnapshot)
		-> std::expected<void, Durin::UnifiedError::FError>
	{
		FCapturedProjectDefaultLevel State;
		auto Result = CaptureProjectDefaultLevel(
			ProjectResolver, State);
		if (!Result) return Result;
		OutSnapshot = {
			.ProviderId = std::string(ProviderId),
			.ProviderVersion = ProviderVersion,
			.Fingerprint = std::move(State.Fingerprint)};
		if (State.Path.IsValid())
			OutSnapshot.Occurrences.push_back({
				.ProviderId = std::string(ProviderId),
				.StableId = std::string(StableId),
				.TargetPath = State.Path,
				.DisplayRoute = "Configs/Project.yaml:Game.DefaultLevel",
				.ExpectedClass = DLevel::StaticClass()->GetQualifiedName().ToString(),
				.bCookRoot = true});
		return {};
	}

	auto FProjectDefaultLevelReferenceStore::PrepareRewrite(
		std::span<const FAssetReferenceRewrite> Rewrites,
		std::string_view ExpectedFingerprint,
		FAssetReferenceStoreRewriteContribution& OutContribution)
		-> std::expected<void, UnifiedError::FError>
	{
		OutContribution = {};
		FCapturedProjectDefaultLevel PreState;
		if (auto Prepared = CaptureProjectDefaultLevel(
			ProjectResolver, PreState); !Prepared)
			return std::unexpected(std::move(Prepared.error()));
		if (ExpectedFingerprint != PreState.Fingerprint
			|| Rewrites.size() != 1
			|| Rewrites.front().StableId != StableId
			|| Rewrites.front().SourcePath != PreState.Path
			|| !Rewrites.front().DestinationPath.IsValid())
			return StoreError(
				AssetWriteErrors::StaleData,
				"Project default-level settings changed before Fix Up preparation.");

		std::error_code StatusError;
		const std::filesystem::perms Permissions = std::filesystem::status(
			PreState.SettingsFile, StatusError).permissions();
		constexpr auto WritePermissions = std::filesystem::perms::owner_write
			| std::filesystem::perms::group_write
			| std::filesystem::perms::others_write;
		if (StatusError)
			return std::unexpected(ToError(FFileSystemError{std::format("Failed to inspect: {}.", FFilePath(PreState.SettingsFile).generic_string()), StatusError}));
		if ((Permissions & WritePermissions)
			== std::filesystem::perms::none)
			return StoreError(
				AssetWriteErrors::ReadOnlyMode,
				"Project settings are read-only and cannot be fixed up.");

		FByteBuffer UpdatedBytes;
		const FProjectGameSettingsResult UpdateResult =
			FProjectGameSettingsStore(PreState.SettingsFile).BuildDefaultLevelUpdate(
				Rewrites.front().DestinationPath.ToString(), UpdatedBytes);
		if (!UpdateResult)
			return StoreError(
				AssetWriteErrors::InvalidData,
				UpdateResult.Message);
		auto PostBytes = std::make_shared<FByteBuffer>(
			std::move(UpdatedBytes));
		const FPackagePath PrePath = PreState.Path;
		const FPackagePath PostPath = Rewrites.front().DestinationPath;
		const std::filesystem::path SettingsFile = PreState.SettingsFile;
		const std::string PreFingerprint = PreState.Fingerprint;
		const std::string PostFingerprint = MakeFingerprint(
			SettingsFile, true, *PostBytes);
		const FPathChanged NotifyPathChanged = PathChanged;
		const FProjectResolver ResolveProject = ProjectResolver;

		OutContribution = {
			.Fingerprint = PreFingerprint,
			.Rewrites = {Rewrites.front()},
			.Revalidate = [SettingsFile, PreFingerprint, PrePath, ResolveProject]() -> std::expected<void, UnifiedError::FError> {
				FCapturedProjectDefaultLevel Current;
				const auto CurrentResult = CaptureProjectDefaultLevel(ResolveProject, Current);
				if (!CurrentResult) return std::unexpected(CurrentResult.error());
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PreFingerprint
					&& Current.Path == PrePath
					? std::expected<void, UnifiedError::FError>{}
					: StoreError(
						AssetWriteErrors::StaleData,
						"Project settings changed after Fix Up analysis.");
			},
			.Apply = [SettingsFile, PostBytes, PostPath, NotifyPathChanged](std::optional<UnifiedError::FError>* OutError) {
				const bool SaveResult = SaveSettingsBytes(
					SettingsFile, *PostBytes, OutError);
				if (SaveResult && NotifyPathChanged) NotifyPathChanged(PostPath);
				return SaveResult;
			},
			.Verify = [SettingsFile, PostFingerprint, PostPath, ResolveProject]() -> std::expected<void, UnifiedError::FError> {
				FCapturedProjectDefaultLevel Current;
				const auto CurrentResult = CaptureProjectDefaultLevel(ResolveProject, Current);
				if (!CurrentResult) return std::unexpected(CurrentResult.error());
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PostFingerprint
					&& Current.Path == PostPath
					? std::expected<void, UnifiedError::FError>{}
					: StoreError(
						AssetWriteErrors::StaleData,
						"Project settings did not retain the Fix Up rewrite.");
			}};
		return {};
	}
}
