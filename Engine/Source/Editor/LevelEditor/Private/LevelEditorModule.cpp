#include "LevelEditorModule.h"

#include "AssetAuthoring.h"
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
#include "TerrainHeightmapAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "GrayboxSceneAuthoring.h"
#include "Misc/StartupCommand.h"
#include "Asset/Load.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
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

		auto ReimportTerrainHeightmap(
			const Editor::ContentBrowser::FExtensionInvocation& Invocation) -> void
		{
			auto ReportError = [&Invocation](std::string Message) {
				if (Invocation.ReportError)
					Invocation.ReportError(std::move(Message));
			};
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Invocation.Context.AssetPath, Path))
			{
				ReportError("The selected Terrain Heightmap path is invalid.");
				return;
			}
			DTerrainHeightmap* Heightmap = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Path, Heightmap);
			if (!Load || !Heightmap)
			{
				ReportError(Load ? "The selected Terrain Heightmap could not be loaded."
					: Load.Message);
				return;
			}
			AssetForge::FImportProvenance Existing;
			AssetForge::FImportRequest Request;
			std::string Error;
			if (!AssetForge::Builtins::InspectTerrainHeightmapImportProvenance(
				*Heightmap, Existing, Error)
				|| !AssetForge::Builtins::MakeTerrainHeightmapImportRequest(
					Heightmap->GetSourceImportData().SourcePath, Path,
					AssetForge::EImportMode::Reimport,
					{.OwnerId = std::format("LevelEditor.Reimport:{}", Path.ToString()),
						.ConflictIdentities = {Path.ToString()}},
					std::move(Existing), Request, Error))
			{
				ReportError(std::move(Error));
				return;
			}
			if (!Invocation.SubmitImport)
			{
				ReportError("The Content Browser import submitter is unavailable.");
				return;
			}
			(void)Invocation.SubmitImport(std::move(Request),
				std::format("Reimport {}", Path.GetAssetName()));
		}
	}

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
		::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService,
		std::function<void(AssetForge::FImportOperationHandle, std::string)>
			NotifyImportStarted,
		Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks) -> bool
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
			ThumbnailOperations.GetTaskScope(), std::move(NotifyImportStarted),
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
			::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>(
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
			ContentBrowserExtensions.push_back(std::move(Handle));
		}
		const auto RegisterImport = [this, WeakWorkspace = LevelEditorWorkspace](
			std::string Id, std::string Label,
			Editor::Level::EImportDialogType Type) {
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterExtension({
				.Id = std::move(Id),
				.Label = std::move(Label),
				.Category = Editor::ContentBrowser::EExtensionCategory::Import,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [WeakWorkspace, Type](const auto& Invocation) {
					if (const std::shared_ptr<MLevelEditor> Pinned = WeakWorkspace.lock())
						Pinned->RequestContentBrowserImport(
							Invocation.Context.VirtualDirectory, Type);
				},
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser import extension: {}", Error);
				return false;
			}
			ContentBrowserExtensions.push_back(std::move(Handle));
			return true;
		};
		if (!RegisterImport("level.terrain-heightmap-import", "Terrain Heightmap...",
				Editor::Level::EImportDialogType::TerrainHeightmap)
			|| !RegisterImport("level.scene-import", "Scene Source (FBX/glTF)...",
				Editor::Level::EImportDialogType::Scene))
		{
			ContentBrowserExtensions.clear();
			LevelEditorWorkspace.reset();
			TerrainThumbnailRegistration.reset();
			WorkspaceRegistration.reset();
			return false;
		}
		{
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterExtension({
				.Id = "level.terrain-heightmap-reimport",
				.Label = "Reimport from Current Source",
				.Category = Editor::ContentBrowser::EExtensionCategory::Reimport,
				.Order = 100,
				.IsApplicable = [](const auto& Context) {
					return Context.AssetClassName
						== DTerrainHeightmap::StaticClass()->GetQualifiedName().ToString();
				},
				.Invoke = ReimportTerrainHeightmap,
				.OwnerGate = EditorExtensionCallbacks.GetGate(),
			}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser Terrain Heightmap reimport: {}", Error);
				ContentBrowserExtensions.clear();
				LevelEditorWorkspace.reset();
				TerrainThumbnailRegistration.reset();
				WorkspaceRegistration.reset();
				return false;
			}
			ContentBrowserExtensions.push_back(std::move(Handle));
		}
		return true;
	}

	LEVELEDITOR_API auto FLevelEditorModule::UnregisterLevelEditorWorkspace() -> void
	{
		ContentBrowserExtensions.clear();
		TerrainThumbnailRegistration.reset();
		WorkspaceRegistration.reset();
		LevelEditorWorkspace.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::OpenDefaultDocument() -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		return Workspace && Workspace->OpenDefaultDocument();
	}


}
