#include "LevelEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Engine/Level.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Widgets/MLevelEditor.h"
#include "Customizations/CameraEditorCustomizations.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SplineComponent.h"
#include "Customizations/DirectionalLightEditorCustomizations.h"
#include "Customizations/SplineEditorCustomizations.h"
#include "StaticMeshMaterialSlotDetails.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyBoxComponent.h"
#include "SkyBoxDetails.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	FLevelEditorModule::~FLevelEditorModule() = default;

	LEVELEDITOR_API auto FLevelEditorModule::StartupModule() -> void
	{
		SessionSettings = std::make_unique<FLevelEditorSessionSettings>();
		SessionSettings->Load();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DCameraComponent::StaticClass(), CreateCameraDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DStaticMeshComponent::StaticClass(), CreateStaticMeshComponentDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSkyBoxComponent::StaticClass(), CreateSkyBoxDetailsCustomization()));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		UnregisterLevelEditorWorkspace();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		for (auto It = CustomizationHandles.rbegin(); It != CustomizationHandles.rend(); ++It) Registry.Unregister(*It);
		CustomizationHandles.clear();
		SessionSettings.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RegisterLevelEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(*SessionSettings, WorkspaceManager);
		Workspace->Construct();
		FEditorWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = LevelEditorWorkspace::Type,
						.DisplayName = "Level Editor",
						.RootKey = LevelEditorWorkspace::RootKey,
						.bShowInWindowMenu = true,
						.bOpenByDefault = true,
						.DefaultHostDockPreference = EEditorWorkspaceHostDockPreference::Center,
						.SingletonDocumentKey = "LevelEditor",
						.SingletonDocumentLabel = "Level Editor",
						.bSingletonDocumentClosable = true,
					},
					.Workspace = Workspace,
				},
			},
			.AssetEditors = {
				{
					.AssetClassName = DLevel::StaticClass()->GetQualifiedName().ToString(),
					.WorkspaceType = LevelEditorWorkspace::Type,
					.DocumentPolicy = EEditorDocumentPolicy::Singleton,
					.SingletonDocumentKey = "LevelEditor",
					.SingletonLabel = "Level Editor",
					.bClosable = true,
				},
			},
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<FEditorWorkspaceRegistrationHandle>(std::move(Registration));
		return true;
	}

	LEVELEDITOR_API auto FLevelEditorModule::UnregisterLevelEditorWorkspace() -> void
	{
		WorkspaceRegistration.reset();
	}
}
