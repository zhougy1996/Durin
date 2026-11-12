#include "Editor/EditorStartup.h"

#include "Asset/Asset.h"
#include "DObject/AssetPath.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/Level.h"
#include "Project/Project.h"

namespace Durin::EditorStartup
{
	auto ResolveStartupLevel(const FProjectInfo& Project, FSoftObjectPath& OutPath, std::string& OutError) -> bool
	{
		OutPath = {};
		OutError.clear();
		FProjectGameSettings Settings;
		if (const auto Result = FProjectGameSettingsStore::ForProject(Project).Load(Settings); !Result)
		{
			OutError = std::format("Failed to load project game settings: {}", Result.Message);
			return false;
		}
		if (Settings.DefaultLevel.empty()) return true;
		FPackagePath PackagePath;
		if (const auto Result = FPackagePath::TryCreateWithDiagnostic(Settings.DefaultLevel, PackagePath); !Result)
		{
			OutError = std::format("Project default level '{}' is invalid: {}", Settings.DefaultLevel, ToString(Result.error()));
			return false;
		}
		if (const auto Result = ResolveLevelPackage(PackagePath, OutPath); !Result)
		{
			OutError = std::format("Project default level '{}' is invalid: {}", Settings.DefaultLevel, Result.Message);
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
