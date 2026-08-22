#include "Engine/ProjectGameSettings.h"

#include "Actors/GameMode.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"
#include "Modules/ModuleManager.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view SettingsRoute = "Configs/Project.yaml:Game";

		auto Failure(EProjectGameSettingsError Error, std::string Message) -> FProjectGameSettingsResult
		{
			return {Error, std::move(Message)};
		}

		auto LoadDocument(const std::filesystem::path& File, FYamlDocument& Document) -> FProjectGameSettingsResult
		{
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(File, ExistsError);
			if (ExistsError)
				return Failure(EProjectGameSettingsError::IoError, std::format("Could not inspect project game settings '{}': {}", File.generic_string(), ExistsError.message()));
			if (!bExists) return {};
			FYamlParseError ParseError;
			if (!Document.LoadFromFile(File.generic_string(), &ParseError))
				return Failure(EProjectGameSettingsError::MalformedYaml, std::format("Project game settings '{}' are malformed: {}", File.generic_string(), ParseError.Message));
			if (!Document.GetRootView().IsMap())
				return Failure(EProjectGameSettingsError::InvalidRoot, std::format("Project game settings '{}' must contain a YAML map at the root.", File.generic_string()));
			return {};
		}

		auto ReadOptionalScalar(const FYamlNodeView& Section, std::string_view Key, std::string& OutValue) -> bool
		{
			const FYamlNodeView Value = Section.GetView(Key);
			if (!Value.IsValid())
			{
				OutValue.clear();
				return true;
			}
			return Value.GetValue(OutValue);
		}
	}

	FProjectGameSettingsStore::FProjectGameSettingsStore(std::filesystem::path InSettingsFile)
		: SettingsFile(std::move(InSettingsFile))
	{
	}

	auto FProjectGameSettingsStore::ForProject(const FProjectInfo& Project) -> FProjectGameSettingsStore
	{
		return FProjectGameSettingsStore(std::filesystem::path(Project.ProjectDir) / "Configs" / "Project.yaml");
	}

	auto FProjectGameSettingsStore::Load(FProjectGameSettings& OutSettings) const -> FProjectGameSettingsResult
	{
		OutSettings = {};
		OutSettings.SettingsFile = SettingsFile.generic_string();
		FYamlDocument Document;
		FProjectGameSettingsResult Result = LoadDocument(SettingsFile, Document);
		if (!Result || !Document.IsValid()) return Result;
		const FYamlNodeView Game = Document.GetRootView().GetView("Game");
		if (!Game.IsValid()) return {};
		if (!Game.IsMap())
			return Failure(EProjectGameSettingsError::InvalidGameSection, std::format("{} must be a YAML map.", SettingsRoute));
		if (!ReadOptionalScalar(Game, "DefaultLevel", OutSettings.DefaultLevel)
			|| !ReadOptionalScalar(Game, "NativeModule", OutSettings.NativeModule)
			|| !ReadOptionalScalar(Game, "GameModeClass", OutSettings.GameModeClass))
		{
			return Failure(EProjectGameSettingsError::InvalidScalar,
				std::format("{}.DefaultLevel, {}.NativeModule, and {}.GameModeClass must be YAML scalars.",
					SettingsRoute, SettingsRoute, SettingsRoute));
		}
		if (OutSettings.NativeModule.empty() != OutSettings.GameModeClass.empty())
		{
			return Failure(EProjectGameSettingsError::IncompleteNativeGameplayPair,
				std::format("{} requires both Game.NativeModule and Game.GameModeClass; module='{}', class='{}'.",
					SettingsRoute, OutSettings.NativeModule, OutSettings.GameModeClass));
		}
		return {};
	}

	auto FProjectGameSettingsStore::BuildDefaultLevelUpdate(
		std::string_view DefaultLevel,
		std::vector<std::byte>& OutBytes) const -> FProjectGameSettingsResult
	{
		OutBytes.clear();
		FYamlDocument Document;
		FProjectGameSettingsResult Result = LoadDocument(SettingsFile, Document);
		if (!Result) return Result;
		if (Document.IsValid())
		{
			const FYamlNodeView ExistingGame = Document.GetRootView().GetView("Game");
			if (ExistingGame.IsValid())
			{
				if (!ExistingGame.IsMap())
					return Failure(EProjectGameSettingsError::InvalidGameSection, std::format("{} must be a YAML map.", SettingsRoute));
				std::string ExistingDefaultLevel;
				std::string ExistingNativeModule;
				std::string ExistingGameModeClass;
				if (!ReadOptionalScalar(ExistingGame, "DefaultLevel", ExistingDefaultLevel)
					|| !ReadOptionalScalar(ExistingGame, "NativeModule", ExistingNativeModule)
					|| !ReadOptionalScalar(ExistingGame, "GameModeClass", ExistingGameModeClass))
				{
					return Failure(EProjectGameSettingsError::InvalidScalar,
						std::format("{}.DefaultLevel, {}.NativeModule, and {}.GameModeClass must be YAML scalars.",
							SettingsRoute, SettingsRoute, SettingsRoute));
				}
				if (ExistingNativeModule.empty() != ExistingGameModeClass.empty())
				{
					return Failure(EProjectGameSettingsError::IncompleteNativeGameplayPair,
						std::format("{} requires both Game.NativeModule and Game.GameModeClass; module='{}', class='{}'.",
							SettingsRoute, ExistingNativeModule, ExistingGameModeClass));
				}
			}
		}
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		FYamlNodeRef Game = Root.GetRef("Game");
		if (!Game.IsValid()) Game = Root.AddMap("Game");
		else if (!Game.IsMap())
			return Failure(EProjectGameSettingsError::InvalidGameSection, std::format("{} must be a YAML map.", SettingsRoute));
		Game.SetChildValue("DefaultLevel", DefaultLevel);
		const std::string Serialized = Document.ToString();
		if (Serialized.empty()) return Failure(EProjectGameSettingsError::IoError, "Project game settings serialized to empty bytes.");
		const auto SerializedBytes = std::as_bytes(std::span(Serialized));
		OutBytes.assign(SerializedBytes.begin(), SerializedBytes.end());
		return {};
	}

	auto FProjectGameSettingsStore::SaveDefaultLevel(std::string_view DefaultLevel) const -> FProjectGameSettingsResult
	{
		std::vector<std::byte> Bytes;
		FProjectGameSettingsResult Result = BuildDefaultLevelUpdate(DefaultLevel, Bytes);
		if (!Result) return Result;
		std::error_code DirectoryError;
		std::filesystem::create_directories(SettingsFile.parent_path(), DirectoryError);
		if (DirectoryError)
			return Failure(EProjectGameSettingsError::IoError, std::format("Could not create the project settings directory: {}", DirectoryError.message()));
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()},
				SettingsFile,
				&PublicationError))
		{
			return Failure(EProjectGameSettingsError::IoError, std::format("Could not save project game settings: {}", PublicationError.ToString()));
		}
		return {};
	}

	auto ResolveNativeGameMode(const FProjectGameSettings& Settings) -> FNativeGameModeResolution
	{
		const std::string Route = Settings.SettingsFile.empty()
			? std::string(SettingsRoute)
			: std::format("{}:Game", Settings.SettingsFile);
		if (Settings.NativeModule.empty() && Settings.GameModeClass.empty()) return {};
		if (Settings.NativeModule.empty() || Settings.GameModeClass.empty())
		{
			return {{EProjectGameSettingsError::IncompleteNativeGameplayPair,
				std::format("{} requires a complete native module/class pair; module='{}', class='{}'.",
					Route, Settings.NativeModule, Settings.GameModeClass)}, nullptr};
		}
		if (!FModuleManager::Get().LoadModule(FName(Settings.NativeModule)))
		{
			return {{EProjectGameSettingsError::ModuleLoadFailed,
				std::format("Could not load native gameplay module '{}' for class '{}' from {}.",
					Settings.NativeModule, Settings.GameModeClass, Route)}, nullptr};
		}
		DClass* GameModeClass = FindClassByQualifiedName(FName(Settings.GameModeClass));
		if (!GameModeClass)
		{
			return {{EProjectGameSettingsError::GameModeClassMissing,
				std::format("Native gameplay class '{}' was not registered after loading module '{}' from {}.",
					Settings.GameModeClass, Settings.NativeModule, Route)}, nullptr};
		}
		if (!CanConstructObjectOfClass(GameModeClass, AGameMode::StaticClass()))
		{
			return {{EProjectGameSettingsError::InvalidGameModeClass,
				std::format("Native gameplay class '{}' from module '{}' is not a constructible Durin::AGameMode ({}).",
					Settings.GameModeClass, Settings.NativeModule, Route)}, nullptr};
		}
		return {{}, GameModeClass};
	}
}
