#include "Editor/EditorStartup.h"

#include "Asset/Asset.h"
#include "DObject/AssetPath.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/Level.h"
#include "Plugins/PluginResources.h"
#include "Project/Project.h"
#include "Yaml/Yaml.h"

namespace Durin::EditorStartup
{
	auto ResolveStartupLevel(const FProjectInfo& Project, FSoftObjectPath& OutPath, std::string& OutError) -> bool
	{
		OutPath = {};
		OutError.clear();
		const auto GameSettings = FProjectGameSettingsStore::ForProject(Project);
		FYamlDocument Document;
		if (!LoadProjectConfig(GameSettings.GetSettingsFile(), Document, OutError)) return false;
		const auto Editor = Document.GetRootView().GetView("Editor");
		if (Editor.IsValid() && !Editor.IsMap())
		{
			OutError = "Configs/Project.yaml:Editor must be a YAML map.";
			return false;
		}
		std::string StartupMap;
		if (const auto Value = Editor.GetView("StartupMap"); Value.IsValid())
		{
			if (!Value.GetValue(StartupMap))
			{
				OutError = "Configs/Project.yaml:Editor.StartupMap must be a YAML scalar.";
				return false;
			}
		}
		else
		{
			// Existing project files used Game.DefaultLevel for both hosts.
			FProjectGameSettings Settings;
			if (const auto Result = GameSettings.Load(Settings); !Result)
			{
				OutError = std::format("Failed to load project game settings: {}", Result.Message);
				return false;
			}
			StartupMap = std::move(Settings.DefaultLevel);
		}
		if (StartupMap.empty()) return true;
		FPackagePath PackagePath;
		if (const auto Result = FPackagePath::TryCreateWithDiagnostic(StartupMap, PackagePath); !Result)
		{
			OutError = std::format("Editor startup map '{}' is invalid: {}", StartupMap, ToString(Result.error()));
			return false;
		}
		if (const auto Result = ResolveLevelPackage(PackagePath, OutPath); !Result)
		{
			OutError = std::format("Editor startup map '{}' is invalid: {}", StartupMap, Result.error());
			return false;
		}
		// Compare a complete mount segment, not a similarly prefixed project name.
		std::string MountRoot = Project.MountRoot;
		if (!MountRoot.ends_with('/')) MountRoot += '/';
		if (Project.MountRoot.empty() || !OutPath.GetPackagePath().GetView().starts_with(MountRoot))
		{
			OutPath = {};
			OutError = "The configured default Level is outside the selected project mount.";
			return false;
		}
		return true;
	}
}
