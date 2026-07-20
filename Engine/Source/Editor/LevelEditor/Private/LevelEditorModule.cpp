#include "LevelEditorModule.h"

#include "Editor/EditorWorkspace.h"
#include "EditorSessionSettings.h"
#include "Engine/Level.h"
#include "LevelEditorWorkspace.h"
#include "MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Widgets/MMaterialEditor.h"
#include "Widgets/MLevelEditor.h"
#include "Actors/CameraActor.h"
#include "CameraEditorCustomizations.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SplineComponent.h"
#include "DirectionalLightEditorCustomizations.h"
#include "SplineEditorCustomizations.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	FLevelEditorModule::~FLevelEditorModule() = default;

	LEVELEDITOR_API auto FLevelEditorModule::StartupModule() -> void
	{
		SessionSettings = std::make_unique<FEditorSessionSettings>();
		SessionSettings->Load();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		const std::shared_ptr<IObjectDetailsCustomization> CameraDetails = CreateCameraDetailsCustomization();
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(ACameraActor::StaticClass(), CameraDetails));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DCameraComponent::StaticClass(), CameraDetails));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization()));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		for (auto It = CustomizationHandles.rbegin(); It != CustomizationHandles.rend(); ++It) Registry.Unregister(*It);
		CustomizationHandles.clear();
		SessionSettings.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RegisterLevelEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool
	{
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(*SessionSettings, WorkspaceManager);
		Workspace->Construct();
		if (!WorkspaceManager.RegisterWorkspace(Workspace)) return false;
		std::shared_ptr<MMaterialEditor> MaterialWorkspace = std::make_shared<MMaterialEditor>(WorkspaceManager);
		if (!WorkspaceManager.RegisterWorkspace(MaterialWorkspace)) return false;
		if (!WorkspaceManager.RegisterAssetEditor({
			.AssetClassName = DLevel::StaticClass()->GetQualifiedName().ToString(),
			.WorkspaceType = LevelEditorWorkspace::Type,
			.DocumentPolicy = EEditorDocumentPolicy::Singleton,
			.SingletonDocumentKey = "LevelEditor",
			.SingletonLabel = "Level Editor",
			.bClosable = false,
		}))
			return false;
		if (!WorkspaceManager.RegisterAssetEditor({
			.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString(),
			.WorkspaceType = MaterialEditorWorkspace::Type,
			.DocumentPolicy = EEditorDocumentPolicy::PerResource,
			.bClosable = true,
		}))
			return false;
		if (!WorkspaceManager.RegisterAssetEditor({
			.AssetClassName = DMaterialInstance::StaticClass()->GetQualifiedName().ToString(),
			.WorkspaceType = MaterialEditorWorkspace::Type,
			.DocumentPolicy = EEditorDocumentPolicy::PerResource,
			.bClosable = true,
		}))
			return false;
		return WorkspaceManager.OpenDocument({
			.WorkspaceType = LevelEditorWorkspace::Type,
			.DocumentKey = "LevelEditor",
			.Label = "Level Editor",
			.bClosable = false,
		}).IsValid();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowWidth() const -> int32
	{
		return SessionSettings->GetWindowWidth();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowHeight() const -> int32
	{
		return SessionSettings->GetWindowHeight();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetUIScale() const -> float
	{
		return SessionSettings->GetUIScale();
	}

	LEVELEDITOR_API auto FLevelEditorModule::IsWindowMaximized() const -> bool
	{
		return SessionSettings->IsWindowMaximized();
	}
}
