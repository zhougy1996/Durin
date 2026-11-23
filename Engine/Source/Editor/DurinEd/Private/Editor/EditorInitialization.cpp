#include "Editor/EditorEngine.h"
#include "Editor/IMainFrameModule.h"
#include "Notifications/Notification.h"
#include "Asset/Asset.h"
#include "DObject/AssetPath.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "Project/Project.h"
#include "Interfaces/IProjectManager.h"
#include "Plugins/PluginManagement.h"
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
		if (const auto Content = IProjectManager::Get().AreProjectModulesAvailable() ? InitializeProjectContent() : std::expected<void, std::string>{}; !Content)
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
        for (const auto& Diagnostic : GetPluginStartupDiagnostics())
            GetNotificationManager().Post({.Type = ENotificationType::Warning, .Message = "A plugin could not be enabled", .Details = Diagnostic});
		AddRegisteredEditorSubsystems();
		if (bShutdownRequested) return false;
		if (!MainFrameModule->InitializeWorkspaces(StartupError)) return false;
		return !bShutdownRequested;
	}

}
