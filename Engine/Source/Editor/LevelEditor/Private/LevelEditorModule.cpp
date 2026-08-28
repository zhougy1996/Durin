#include "LevelEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset.h"
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
#include "VolumetricCloudDetails.h"
#include "Components/VolumetricCloudComponent.h"
#include "TerrainHeightmapThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "GrayboxSceneBuild.h"
#include "Misc/StartupCommand.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	using namespace Editor::Level;
	namespace
	{
		auto CreateLevelAsset(std::string_view VirtualDirectory,
			std::string& OutPath, std::string& OutError) -> bool
		{
			std::string Directory(VirtualDirectory);
			if (!Directory.ends_with('/')) Directory += '/';
			FAssetPath Path;
			for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
			{
				const std::string Name = Suffix == 0
					? "NewLevel" : std::format("NewLevel{}", Suffix + 1);
				if (!FAssetPath::TryCreate(Directory + Name, Path)
					|| Asset::FindAssetExact(Path)
					|| Asset::FindResidentPackage(Path)) continue;
				DLevel* Level = nullptr;
				Asset::FAssetResult Result = Asset::CreateAsset(Path, Level);
				if (!Result || !Level)
				{
					OutError = Result ? "Could not create the level asset." : Result.Message;
					return false;
				}
				Result = Asset::SavePackage(Level->GetPackage());
				if (!Result)
				{
					Asset::UnloadPackage(Path);
					OutError = Result.Message;
					return false;
				}
				OutPath = Path.ToString();
				return true;
			}
			OutError = "Could not find a unique level asset name in this folder.";
			return false;
		}

	}

	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	struct FLevelEditorModule::FIntegrationState
	{
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};

	FLevelEditorModule::FLevelEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}

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
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DVolumetricCloudComponent::StaticClass(), CreateVolumetricCloudDetailsCustomization(), ExtensionGate));
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
		::Durin::Editor::DThumbnailManager& ThumbnailManager,
		Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
		TerrainThumbnailRegistration.reset();
		std::string Error;
		auto ThumbnailHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DTerrainHeightmapThumbnailRenderer>(),
			EditorExtensionCallbacks.GetGate(), Error);
		if (!ThumbnailHandle) return false;

		// Content Browser captures thumbnail routing while the workspace is constructed,
		// so feature-owned renderers must already be visible to the shared service.
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(
			*SessionSettings, WorkspaceManager, EditorExtensionCallbacks.GetGate(),
			ThumbnailOperations.GetTaskScope(),
			std::move(ContentBrowserCallbacks));
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
			::Durin::Editor::FThumbnailRendererRegistrationHandle>(
				std::move(ThumbnailHandle));
		LevelEditorWorkspace = Workspace;
		{
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterExtension({
				.Id = "level.create-level",
				.Label = "Level",
				.Category = Editor::ContentBrowser::EExtensionCategory::Create,
				.Order = 100,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [](const auto& Invocation) {
					std::string Path;
					std::string Error;
					if (!CreateLevelAsset(
						Invocation.Context.VirtualDirectory, Path, Error))
					{
						if (Invocation.ReportError)
							Invocation.ReportError(std::move(Error));
						return;
					}
					if (Invocation.NotifyMountedContentChanged)
						Invocation.NotifyMountedContentChanged();
					if (Invocation.RevealAsset) Invocation.RevealAsset(Path);
				},
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser Level creation: {}", Error);
				LevelEditorWorkspace.reset();
				TerrainThumbnailRegistration.reset();
				WorkspaceRegistration.reset();
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
		}
		return true;
	}

	LEVELEDITOR_API auto FLevelEditorModule::UnregisterLevelEditorWorkspace() -> void
	{
		Integration->ContentBrowserExtensions.clear();
		TerrainThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
		LevelEditorWorkspace.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::OpenDefaultDocument() -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		return Workspace && Workspace->OpenDefaultDocument();
	}

	auto FLevelEditorModule::OpenImportDialog(
		Editor::Level::EImportDialogType Type, std::string_view Directory) -> void
	{
		if (const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock())
			Workspace->RequestContentBrowserImport(std::string(Directory), Type);
	}

}
