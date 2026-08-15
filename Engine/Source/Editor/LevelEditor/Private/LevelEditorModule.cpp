#include "LevelEditorModule.h"

#include "AssetMutation.h"
#include "Editor/WorkspaceManager.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Settings/ProjectDefaultLevelReferenceStore.h"
#include "Engine/Level.h"
#include "Actors/PlayerStart.h"
#include "Actors/SplineMeshActor.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Widgets/MLevelEditor.h"
#include "Customizations/CameraEditorCustomizations.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SplineComponent.h"
#include "Customizations/DirectionalLightEditorCustomizations.h"
#include "Customizations/PlayerStartEditorCustomizations.h"
#include "Customizations/SplineEditorCustomizations.h"
#include "StaticMeshMaterialSlotDetails.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/TerrainComponent.h"
#include "SkyBoxDetails.h"
#include "TerrainDetails.h"
#include "TerrainHeightmapAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "GrayboxSceneAuthoring.h"
#include "Misc/StartupCommand.h"

namespace Durin
{
	using namespace Editor::Level;

	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	FLevelEditorModule::~FLevelEditorModule() = default;

	LEVELEDITOR_API auto FLevelEditorModule::StartupModule() -> void
	{
		EditorExtensionCallbacks =
			FModuleStartup::CreateOwnedCallbackRegistration("Editor.ExtensionRegistries");
		require(EditorExtensionCallbacks.IsValid());
		ThumbnailOperations =
			FModuleStartup::CreateAsyncOperationGroup("SourceImageThumbnail.Decodes");
		require(ThumbnailOperations.IsValid());
		ProjectDefaultLevelReferenceStore =
			std::make_unique<FProjectDefaultLevelReferenceStore>(
				[this](const FAssetPath& Path) {
					if (const std::shared_ptr<MLevelEditor> Workspace =
							LevelEditorWorkspace.lock())
						Workspace->ApplyFixedUpDefaultLevelPath(Path);
				});
		ProjectDefaultLevelReferenceStoreHandle =
			Asset::RegisterAssetReferenceStore(
				ProjectDefaultLevelReferenceStore.get(),
				EditorExtensionCallbacks.GetGate());
		SessionSettings = std::make_unique<FLevelEditorSessionSettings>();
		SessionSettings->Load();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		const FModuleOwnedCallbackGate ExtensionGate = EditorExtensionCallbacks.GetGate();
		CustomizationHandles.push_back(Registry.RegisterActorVisualizer(APlayerStart::StaticClass(), CreatePlayerStartActorVisualizer(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DCameraComponent::StaticClass(), CreateCameraDetailsCustomization(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(ASplineMeshActor::StaticClass(), CreateSplineMeshActorDetailsCustomization(), ExtensionGate));
		SplineEditModeHandle = RegisterSplineViewportEditMode(ExtensionGate);
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DStaticMeshComponent::StaticClass(), CreateStaticMeshComponentDetailsCustomization(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSkyBoxComponent::StaticClass(), CreateSkyBoxDetailsCustomization(), ExtensionGate));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DTerrainComponent::StaticClass(), CreateTerrainDetailsCustomization(), ExtensionGate));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
		GrayboxBuildStartupCommandHandle = RegisterStartupCommandHandler(
			"graybox-build", RunGrayboxBuildStartupCommand,
			EditorExtensionCallbacks.GetGate());
		checkf(GrayboxBuildStartupCommandHandle != 0,
			"LevelEditor graybox-build startup command must register exactly once");
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		UnregisterLevelEditorWorkspace();
		UnregisterStartupCommandHandler(GrayboxBuildStartupCommandHandle);
		GrayboxBuildStartupCommandHandle = 0;
		Asset::UnregisterAssetReferenceStore(
			ProjectDefaultLevelReferenceStoreHandle);
		ProjectDefaultLevelReferenceStoreHandle = 0;
		ProjectDefaultLevelReferenceStore.reset();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		if (SplineEditModeHandle) FLevelViewportEditModeRegistry::Get().Unregister(SplineEditModeHandle);
		SplineEditModeHandle = {};
		for (auto It = CustomizationHandles.rbegin(); It != CustomizationHandles.rend(); ++It) Registry.Unregister(*It);
		CustomizationHandles.clear();
		SessionSettings.reset();
		TerrainThumbnailRegistration.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RegisterLevelEditorWorkspace(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
		TerrainThumbnailRegistration.reset();
		std::string Error;
		auto ThumbnailHandle = ThumbnailService.RegisterScoped(
			std::make_unique<FTerrainHeightmapAssetThumbnailProvider>(),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle) return false;

		// Content Browser captures thumbnail routing while the workspace is constructed,
		// so feature-owned providers must already be visible to the shared service.
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(
			*SessionSettings, WorkspaceManager, EditorExtensionCallbacks.GetGate(),
			ThumbnailOperations.GetTaskScope());
		Workspace->Construct();
		::Durin::Editor::FWorkspaceRegistrationHandle Registration = WorkspaceManager.RegisterBatch({
			.Workspaces = {
				{
					.Descriptor = {
						.WorkspaceType = Workspace::Type,
						.DisplayName = "Level Editor",
						.RootKey = Workspace::RootKey,
						.bShowInWindowMenu = true,
						.bOpenByDefault = true,
						.DefaultHostDockPreference = ::Durin::Editor::EWorkspaceHostDockPreference::Center,
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
					.WorkspaceType = Workspace::Type,
					.DocumentPolicy = ::Durin::Editor::EDocumentPolicy::Singleton,
					.SingletonDocumentKey = "LevelEditor",
					.SingletonLabel = "Level Editor",
					.bClosable = true,
				},
			},
		}, EditorExtensionCallbacks.GetGate());
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		TerrainThumbnailRegistration = std::make_unique<
			::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
				std::move(ThumbnailHandle));
		LevelEditorWorkspace = Workspace;
		return true;
	}

	LEVELEDITOR_API auto FLevelEditorModule::UnregisterLevelEditorWorkspace() -> void
	{
		TerrainThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
		LevelEditorWorkspace.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::OpenDefaultDocument() -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		return Workspace && Workspace->OpenDefaultDocument();
	}

	auto FLevelEditorModule::RevealAssetInContentBrowser(const FAssetPath& AssetPath) -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		return Workspace && Workspace->RevealAssetInContentBrowser(AssetPath);
	}
}
