#include "Editor/EditorEngine.h"
#include "Editor/EditorStartup.h"
#include "Editor/IMainFrameModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Notifications/Notification.h"
#include "Asset/Asset.h"
#include "DObject/AssetPath.h"
#include "DObject/Class.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "Project/Project.h"
#include "Interfaces/IProjectManager.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	auto DEditorEngine::InitializeProjectWorkspace() -> bool
	{
		if (bShutdownRequested) return false;
		if (!StartupError.empty()) return false;
		if (!HasCurrentProject())
		{
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::WorkspaceRegistrationBegin);
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::WorkspaceRegistrationComplete);
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultWorkspaceReady);
			return true;
		}
		// All schema providers must be registered before the first content scan.
		if (!FModuleManager::Get().LoadModule("AssetForgeBuiltins"))
		{
			StartupError = "Project admission requires AssetForgeBuiltins.";
			return false;
		}
        if (auto Modules=IProjectManager::Get().LoadModulesForProject(ELoadingPhase::Providers, [&] { return bShutdownRequested; }); !Modules)
        { StartupError=IProjectManager::Get().GetLastError(); return false; }
		if (bShutdownRequested) return false;
		if (!IProjectManager::Get().LoadModulesForProject(ELoadingPhase::EngineReady, [&] { return bShutdownRequested; }))
		{ StartupError=IProjectManager::Get().GetLastError(); return false; }
		if (const auto Content = InitializeProjectContent(); !Content)
		{
			StartupError = Content.error();
			return false;
		}
		if (bShutdownRequested) return false;
		EditorWorld = GetWorld();
		AddRegisteredEditorSubsystems();
		if (bShutdownRequested) return false;
		return InitializeWorkspaces();
	}

	auto DEditorEngine::InitializeWorkspaces() -> bool
	{
		if (bShutdownRequested) return false;
		StartupError.clear();
		const bool bModulesReady = MainFrameModule->LoadWorkspaceModules();
		if (bShutdownRequested) return false;
		if (!bModulesReady)
		{
			StartupError = "Could not load the editor workspace modules.";
			return false;
		}
		if (!IProjectManager::Get().LoadModulesForProject(ELoadingPhase::EditorReady, [&] { return bShutdownRequested; }))
		{ StartupError=IProjectManager::Get().GetLastError(); return false; }
		AddRegisteredEditorSubsystems();
		if (bShutdownRequested) return false;
		if (!MainFrameModule->InitializeWorkspaces(StartupError)) return false;
		return !bShutdownRequested;
	}

	auto DEditorEngine::OpenDefaultLevel() -> void
	{
		FSoftObjectPath Path;
		if (!EditorStartup::ResolveStartupLevel(*GetCurrentProject(), Path, StartupError))
		{
			DURIN_ERROR("{}", StartupError);
			GetNotificationManager().Post({.Type = ENotificationType::Error,
				.Message = "Could not open the default level", .Details = StartupError});
			return;
		}
		if (!Path.IsValid()) return;
		auto* AssetEditors = GetEditorSubsystem<DAssetEditorSubsystem>();
		requiref(AssetEditors, "Workspace admission requires asset-editor services.");
		InitialLevelPath = Path;
		if (!AssetEditors->OpenAssets({.AssetEditors = AssetEditors,
			.Assets = {{Path, DLevel::StaticClass()->GetQualifiedName().ToString()}}}))
			StartupError = "Could not load the default level.";
	}

}
