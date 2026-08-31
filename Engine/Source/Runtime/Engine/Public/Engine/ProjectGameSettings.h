#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DClass;
	struct FProjectInfo;

	// Holds the single Engine-owned projection of the Configs/Project.yaml Game section.
	struct FProjectGameSettings
	{
		std::string SettingsFile;
		std::string DefaultLevel;
		std::string NativeModule;
		std::string GameModeClass;

		auto HasNativeGameplay() const -> bool { return !NativeModule.empty() && !GameModeClass.empty(); }
	};

	enum class EProjectGameSettingsError : uint8
	{
		None,
		IoError,
		MalformedYaml,
		InvalidRoot,
		InvalidGameSection,
		InvalidScalar,
		IncompleteNativeGameplayPair,
		ModuleLoadFailed,
		GameModeClassMissing,
		InvalidGameModeClass
	};

	struct FProjectGameSettingsResult
	{
		EProjectGameSettingsError Error = EProjectGameSettingsError::None;
		std::string Message;

		explicit operator bool() const { return Error == EProjectGameSettingsError::None; }
	};

	// Parses and updates the authoritative Game section while preserving unrelated YAML settings.
	class FProjectGameSettingsStore
	{
	public:
		ENGINE_API explicit FProjectGameSettingsStore(std::filesystem::path InSettingsFile);
		ENGINE_API static auto ForProject(const FProjectInfo& Project) -> FProjectGameSettingsStore;

		ENGINE_API auto Load(FProjectGameSettings& OutSettings) const -> FProjectGameSettingsResult;
		ENGINE_API auto BuildDefaultLevelUpdate(
			std::string_view DefaultLevel,
			FByteArray& OutBytes) const -> FProjectGameSettingsResult;
		ENGINE_API auto SaveDefaultLevel(std::string_view DefaultLevel) const -> FProjectGameSettingsResult;

		auto GetSettingsFile() const -> const std::filesystem::path& { return SettingsFile; }

	private:
		std::filesystem::path SettingsFile;
	};

	struct FNativeGameModeResolution
	{
		FProjectGameSettingsResult Result;
		DClass* GameModeClass = nullptr;

		explicit operator bool() const { return static_cast<bool>(Result); }
		auto HasNativeGameplay() const -> bool { return GameModeClass != nullptr; }
	};

	// Loads the configured logical module and resolves only the exact qualified AGameMode class.
	ENGINE_API auto ResolveNativeGameMode(const FProjectGameSettings& Settings) -> FNativeGameModeResolution;
}
