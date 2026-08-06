#include "Settings/ProjectDefaultLevelReferenceStore.h"

#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view ProviderId =
			"Durin.LevelEditor.ProjectDefaultLevel";
		constexpr std::string_view StableId = "Editor.DefaultLevel";
		constexpr uint64 ProviderVersion = 1;

		struct FCapturedProjectDefaultLevel
		{
			std::filesystem::path SettingsFile;
			std::vector<uint8> Bytes;
			std::string Fingerprint;
			FAssetPath Path;
			bool bFileExists = false;
		};

		auto StoreError(Asset::EAssetError Error, std::string Message)
			-> Asset::FAssetResult
		{
			return {Error, std::move(Message)};
		}

		auto MakeFingerprint(
			const std::filesystem::path& SettingsFile,
			bool bFileExists,
			std::span<const uint8> Bytes) -> std::string
		{
			std::string Source = std::format(
				"{}\n{}\n{}\n", ProviderId,
				SettingsFile.lexically_normal().generic_string(), bFileExists);
			Source.append(
				reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			return FXxHash128::HashBuffer(std::span{
				reinterpret_cast<const uint8*>(Source.data()), Source.size()})
				.ToString();
		}

		auto CaptureProjectDefaultLevel(
			const FProjectDefaultLevelReferenceStore::FProjectResolver&
				ProjectResolver,
			FCapturedProjectDefaultLevel& OutState) -> Asset::FAssetResult
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
					Asset::EAssetError::IoError,
					std::format("Could not inspect project settings: {}",
						ExistsError.message()));
			if (!OutState.bFileExists)
			{
				OutState.Fingerprint = MakeFingerprint(
					OutState.SettingsFile, false, {});
				return {};
			}
			if (!FFileHelper::LoadFileToArray(
					OutState.Bytes, OutState.SettingsFile.generic_string()))
				return StoreError(
					Asset::EAssetError::IoError,
					"Could not read project settings for redirector Fix Up.");
			FYamlDocument Document;
			FYamlParseError ParseError;
			if (!Document.LoadFromFile(
					OutState.SettingsFile.generic_string(), &ParseError))
				return StoreError(
					Asset::EAssetError::CorruptFile,
					std::format("Project settings are malformed: {}",
						ParseError.Message));
			const std::string StoredPath = Document.GetRootView()
				.GetView("Editor").GetView("DefaultLevel").GetString();
			if (!StoredPath.empty())
			{
				std::string PathError;
				if (!FAssetPath::TryCreate(StoredPath, OutState.Path, &PathError))
					return StoreError(
						Asset::EAssetError::InvalidPath,
						std::format("Project default level is invalid: {}",
							PathError));
			}
			OutState.Fingerprint = MakeFingerprint(
				OutState.SettingsFile, true, OutState.Bytes);
			return {};
		}

		auto SaveSettingsBytes(
			const std::filesystem::path& SettingsFile,
			std::span<const uint8> Bytes) -> Asset::FAssetResult
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (Bytes.empty() || !FFileHelper::SaveArrayToFileAtomically(
					std::span{
						reinterpret_cast<const std::byte*>(Bytes.data()),
						Bytes.size()},
					SettingsFile, &PublicationError))
				return StoreError(
					Asset::EAssetError::IoError,
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
		Asset::FAssetReferenceStoreSnapshot& OutSnapshot)
		-> Asset::FAssetResult
	{
		FCapturedProjectDefaultLevel State;
		Asset::FAssetResult Result = CaptureProjectDefaultLevel(
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
				.DisplayRoute = "Configs/Project.yaml:Editor.DefaultLevel"});
		return {};
	}

	auto FProjectDefaultLevelReferenceStore::PrepareRewrite(
		std::span<const Asset::FAssetReferenceRewrite> Rewrites,
		std::string_view ExpectedFingerprint,
		Asset::FAssetReferenceStoreRewriteContribution& OutContribution)
		-> Asset::FAssetResult
	{
		OutContribution = {};
		FCapturedProjectDefaultLevel PreState;
		Asset::FAssetResult Result = CaptureProjectDefaultLevel(
			ProjectResolver, PreState);
		if (!Result) return Result;
		if (ExpectedFingerprint != PreState.Fingerprint
			|| Rewrites.size() != 1
			|| Rewrites.front().StableId != StableId
			|| Rewrites.front().SourcePath != PreState.Path
			|| !Rewrites.front().DestinationPath.IsValid())
			return StoreError(
				Asset::EAssetError::StaleData,
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
				Asset::EAssetError::ReadOnlyMode,
				"Project settings are read-only and cannot be fixed up.");

		FYamlDocument Document;
		FYamlParseError ParseError;
		if (!Document.LoadFromFile(
				PreState.SettingsFile.generic_string(), &ParseError))
			return StoreError(
				Asset::EAssetError::CorruptFile,
				std::format("Project settings are malformed: {}",
					ParseError.Message));
		if (!Document.GetRootView().IsMap())
			return StoreError(
				Asset::EAssetError::CorruptFile,
				"Project settings must contain a YAML map at the root.");
		FYamlNodeRef Root = Document.GetMutableRoot();
		FYamlNodeRef Editor = Root.GetRef("Editor");
		if (!Editor.IsValid()) Editor = Root.AddMap("Editor");
		else if (!Editor.IsMap())
			return StoreError(
				Asset::EAssetError::CorruptFile,
				"The Editor project setting must be a YAML map.");
		Editor.SetChildValue(
			"DefaultLevel", Rewrites.front().DestinationPath.ToString());
		const std::string Serialized = Document.ToString();
		if (Serialized.empty())
			return StoreError(
				Asset::EAssetError::IoError,
				"Could not serialize project settings for Fix Up.");
		auto PostBytes = std::make_shared<std::vector<uint8>>(
			reinterpret_cast<const uint8*>(Serialized.data()),
			reinterpret_cast<const uint8*>(Serialized.data()) + Serialized.size());
		auto PreBytes = std::make_shared<std::vector<uint8>>(
			std::move(PreState.Bytes));
		const FAssetPath PrePath = PreState.Path;
		const FAssetPath PostPath = Rewrites.front().DestinationPath;
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
				Asset::FAssetResult CurrentResult =
					CaptureProjectDefaultLevel(ResolveProject, Current);
				if (!CurrentResult) return CurrentResult;
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PreFingerprint
					&& Current.Path == PrePath
					? Asset::FAssetResult{}
					: StoreError(
						Asset::EAssetError::StaleData,
						"Project settings changed after Fix Up analysis.");
			},
			.Apply = [SettingsFile, PostBytes, PostPath, NotifyPathChanged] {
				Asset::FAssetResult SaveResult = SaveSettingsBytes(
					SettingsFile, *PostBytes);
				if (SaveResult && NotifyPathChanged) NotifyPathChanged(PostPath);
				return SaveResult;
			},
			.Restore = [SettingsFile, PreBytes, PrePath, NotifyPathChanged] {
				Asset::FAssetResult SaveResult = SaveSettingsBytes(
					SettingsFile, *PreBytes);
				if (SaveResult && NotifyPathChanged) NotifyPathChanged(PrePath);
				return SaveResult;
			},
			.Verify = [SettingsFile, PostFingerprint, PostPath, ResolveProject] {
				FCapturedProjectDefaultLevel Current;
				Asset::FAssetResult CurrentResult =
					CaptureProjectDefaultLevel(ResolveProject, Current);
				if (!CurrentResult) return CurrentResult;
				return Current.SettingsFile == SettingsFile
					&& Current.Fingerprint == PostFingerprint
					&& Current.Path == PostPath
					? Asset::FAssetResult{}
					: StoreError(
						Asset::EAssetError::StaleData,
						"Project settings did not retain the Fix Up rewrite.");
			}};
		return {};
	}
}
