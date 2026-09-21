#include "Settings/ProjectDefaultLevelReferenceStore.h"

#include "Hash/XxHash.h"
#include "DObject/Class.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"

namespace Durin::Editor::Level
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

		auto StoreError(EAssetReadError Error, std::string Message)
			-> FAssetReadResult
		{
			return {Error, std::move(Message)};
		}

		auto StoreError(EAssetWriteError Error, std::string Message)
			-> FAssetWriteResult
		{
			return {Error, std::move(Message)};
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
			FCapturedProjectDefaultLevel& OutState) -> FAssetReadResult
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
				return StoreError(
					EAssetReadError::IoError,
					std::format("Could not inspect project settings: {}",
						ExistsError.message()));
			if (!OutState.bFileExists)
			{
				OutState.Fingerprint = MakeFingerprint(
					OutState.SettingsFile, false, {});
				return {};
			}
			if (!FFileHelper::LoadFileToArray(
					OutState.Bytes, OutState.SettingsFile))
				return StoreError(
					EAssetReadError::IoError,
					"Could not read project settings for redirector Fix Up.");
			FProjectGameSettings Settings;
			const FProjectGameSettingsResult SettingsResult =
				FProjectGameSettingsStore(OutState.SettingsFile).Load(Settings);
			if (!SettingsResult)
				return StoreError(
					EAssetReadError::CorruptFile,
					SettingsResult.Message);
			if (!Settings.DefaultLevel.empty())
			{
				std::string PathError;
				if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, OutState.Path); !PathValidation)
				{
					PathError = Durin::FormatObjectError(PathValidation.Error);
					return StoreError(
						EAssetReadError::InvalidPath,
						std::format("Project default level is invalid: {}",
							PathError));
				}
			}
			OutState.Fingerprint = MakeFingerprint(
				OutState.SettingsFile, true, OutState.Bytes);
			return {};
		}

		auto SaveSettingsBytes(
			const std::filesystem::path& SettingsFile,
			FByteView Bytes) -> FAssetWriteResult
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (Bytes.empty() || !FFileHelper::SaveArrayToFileAtomically(
					std::span{
						reinterpret_cast<const std::byte*>(Bytes.data()),
						Bytes.size()},
					SettingsFile, &PublicationError))
				return StoreError(
					EAssetWriteError::IoError,
					Bytes.empty()
						? "Project settings serialized to empty bytes."
						: PublicationError.ToString());
			return {};
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
		-> FAssetReadResult
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
		-> FAssetWriteResult
	{
		OutContribution = {};
		FCapturedProjectDefaultLevel PreState;
		FAssetWriteResult Result = AssetWriteResultFromRead(CaptureProjectDefaultLevel(
			ProjectResolver, PreState));
		if (!Result) return Result;
		if (ExpectedFingerprint != PreState.Fingerprint
			|| Rewrites.size() != 1
			|| Rewrites.front().StableId != StableId
			|| Rewrites.front().SourcePath != PreState.Path
			|| !Rewrites.front().DestinationPath.IsValid())
			return StoreError(
				EAssetWriteError::StaleData,
				"Project default-level settings changed before Fix Up preparation.");

		std::error_code StatusError;
		const std::filesystem::perms Permissions = std::filesystem::status(
			PreState.SettingsFile, StatusError).permissions();
		constexpr auto WritePermissions = std::filesystem::perms::owner_write
			| std::filesystem::perms::group_write
			| std::filesystem::perms::others_write;
		if (StatusError || (Permissions & WritePermissions)
			== std::filesystem::perms::none)
			return StoreError(
				EAssetWriteError::ReadOnlyMode,
				"Project settings are read-only and cannot be fixed up.");

		FByteBuffer UpdatedBytes;
		const FProjectGameSettingsResult UpdateResult =
			FProjectGameSettingsStore(PreState.SettingsFile).BuildDefaultLevelUpdate(
				Rewrites.front().DestinationPath.ToString(), UpdatedBytes);
		if (!UpdateResult)
			return StoreError(
				EAssetWriteError::InvalidData,
				UpdateResult.Message);
		auto PostBytes = std::make_shared<FByteBuffer>(
			std::move(UpdatedBytes));
		auto PreBytes = std::make_shared<FByteBuffer>(
			std::move(PreState.Bytes));
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
			.Revalidate = [SettingsFile, PreFingerprint, PrePath, ResolveProject] {
				FCapturedProjectDefaultLevel Current;
				FAssetWriteResult CurrentResult =
					AssetWriteResultFromRead(CaptureProjectDefaultLevel(ResolveProject, Current));
				if (!CurrentResult) return CurrentResult;
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PreFingerprint
					&& Current.Path == PrePath
					? FAssetWriteResult{}
					: StoreError(
						EAssetWriteError::StaleData,
						"Project settings changed after Fix Up analysis.");
			},
			.Apply = [SettingsFile, PostBytes, PostPath, NotifyPathChanged] {
				FAssetWriteResult SaveResult = SaveSettingsBytes(
					SettingsFile, *PostBytes);
				if (SaveResult && NotifyPathChanged) NotifyPathChanged(PostPath);
				return SaveResult;
			},
			.Restore = [SettingsFile, PreBytes, PrePath, NotifyPathChanged] {
				FAssetWriteResult SaveResult = SaveSettingsBytes(
					SettingsFile, *PreBytes);
				if (SaveResult && NotifyPathChanged) NotifyPathChanged(PrePath);
				return SaveResult;
			},
			.Verify = [SettingsFile, PostFingerprint, PostPath, ResolveProject] {
				FCapturedProjectDefaultLevel Current;
				FAssetWriteResult CurrentResult =
					AssetWriteResultFromRead(CaptureProjectDefaultLevel(ResolveProject, Current));
				if (!CurrentResult) return CurrentResult;
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PostFingerprint
					&& Current.Path == PostPath
					? FAssetWriteResult{}
					: StoreError(
						EAssetWriteError::StaleData,
						"Project settings did not retain the Fix Up rewrite.");
			}};
		return {};
	}
}
