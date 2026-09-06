#include "LevelEditorModule.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
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
#include "VolumetricCloudDetails.h"
#include "Components/VolumetricCloudComponent.h"

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
			FPackagePath Path;
			for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
			{
				const std::string Name = Suffix == 0
					? "NewLevel" : std::format("NewLevel{}", Suffix + 1);
				if (!FPackagePath::TryCreate(Directory + Name, Path)
					|| FindAssetExact(Path)
					|| FindResidentPackage(Path)) continue;
				FTopLevelAssetPath AssetPath;
				if (!FTopLevelAssetPath::TryCreate(Path, Name, AssetPath)) continue;
				const FAssetToolsResult Created = IAssetTools::Get().CreateAsset(
					AssetPath, DLevel::StaticClass());
				DLevel* Level = Cast<DLevel>(Created.Asset);
				if (!Created || !Level)
				{
					OutError = Created.Message.empty()
						? "Could not create the level asset." : Created.Message;
					return false;
				}
				const FAssetResult Result = SavePackage(Level->GetPackage());
				if (!Result)
				{
					UnloadPackage(Path);
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
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration> TypePresentations;
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
		ThumbnailOperations =
			FModuleStartup::CreateAsyncOperationGroup("SourceImageThumbnail.Decodes");
		require(ThumbnailOperations.IsValid());
		ProjectDefaultLevelReferenceStore =
			std::make_unique<FProjectDefaultLevelReferenceStore>(
				[this](const FPackagePath& Path) {
					if (const std::shared_ptr<MLevelEditor> Workspace =
							LevelEditorWorkspace.lock())
						Workspace->ApplyFixedUpDefaultLevelPath(Path);
				});
		ProjectDefaultLevelReferenceStoreHandle =
			RegisterAssetReferenceStore(
				ProjectDefaultLevelReferenceStore.get());
		SessionSettings = std::make_unique<FLevelEditorSessionSettings>();
		SessionSettings->Load();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();

		CustomizationHandles.push_back(Registry.RegisterActorVisualizer(APlayerStart::StaticClass(), CreatePlayerStartActorVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DCameraComponent::StaticClass(), CreateCameraDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(ASplineMeshActor::StaticClass(), CreateSplineMeshActorDetailsCustomization()));
		SplineEditModeHandle = RegisterSplineViewportEditMode();
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DStaticMeshComponent::StaticClass(), CreateStaticMeshComponentDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterObjectDetails(DVolumetricCloudComponent::StaticClass(), CreateVolumetricCloudDetailsCustomization()));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		UnregisterLevelEditorWorkspace();
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("LevelEditor"),
				"LevelEditor cannot retire while one of its custom changes is active");
		UnregisterAssetReferenceStore(
			ProjectDefaultLevelReferenceStoreHandle);
		ProjectDefaultLevelReferenceStoreHandle = 0;
		ProjectDefaultLevelReferenceStore.reset();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		if (SplineEditModeHandle) FLevelViewportEditModeRegistry::Get().Unregister(SplineEditModeHandle);
		SplineEditModeHandle = {};
		for (auto It = CustomizationHandles.rbegin(); It != CustomizationHandles.rend(); ++It) Registry.Unregister(*It);
		CustomizationHandles.clear();
		SessionSettings.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RegisterLevelEditorWorkspace(
		::Durin::Editor::FWorkspaceManager& WorkspaceManager,
		::Durin::Editor::DThumbnailManager&,
		Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks,
		std::function<void()> OpenProjectBrowser) -> bool
	{
		if (WorkspaceRegistration && WorkspaceRegistration->IsValid()) return false;
		WorkspaceRegistration.reset();
		std::shared_ptr<MLevelEditor> Workspace = std::make_shared<MLevelEditor>(
			*SessionSettings, WorkspaceManager, ThumbnailOperations.GetTaskScope(),
			std::move(ContentBrowserCallbacks), std::move(OpenProjectBrowser));
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
		});
		if (!Registration) return false;
		WorkspaceRegistration = std::make_unique<::Durin::Editor::FWorkspaceRegistrationHandle>(std::move(Registration));
		LevelEditorWorkspace = Workspace;
		{
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterExtension({
				.Id = "level.create-level",
				.Label = "Level",
				.Category = Editor::ContentBrowser::EExtensionCategory::Create,
				.Order = 100,
				.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
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
				}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser Level creation: {}", Error);
				LevelEditorWorkspace.reset();
				WorkspaceRegistration.reset();
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
		}
		const auto RegisterImport = [this, Workspace](
			std::string Id, std::string Label, int32 Order,
			Editor::Level::EImportDialogType Type) -> bool {
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterExtension({
				.Id = std::move(Id),
				.Label = std::move(Label),
				.Category = Editor::ContentBrowser::EExtensionCategory::Import,
				.Order = Order,
				.Mutation = ::Durin::Editor::ContentBrowser::EContentMutation::MutatesContent,
				.IsApplicable = [](const auto& Context) {
					return !Context.VirtualDirectory.empty();
				},
				.Invoke = [WeakWorkspace = std::weak_ptr(Workspace), Type](
					const auto& Invocation) {
					if (const std::shared_ptr<MLevelEditor> AdmittedWorkspace =
						WeakWorkspace.lock())
						AdmittedWorkspace->RequestContentBrowserImport(
							Invocation.Context.VirtualDirectory, Type);
				},
				.DrawHostPresentation = [WeakWorkspace = std::weak_ptr(Workspace), Type](
					bool bAllowAssetMutation) {
					if (const std::shared_ptr<MLevelEditor> AdmittedWorkspace =
						WeakWorkspace.lock())
						AdmittedWorkspace->DrawContentBrowserImport(
							Type, bAllowAssetMutation);
				},
				}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser import: {}", Error);
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
			return true;
		};
		if (!RegisterImport(
				"level.import-scene", "Scene Source (FBX/glTF)...", 300,
				Editor::Level::EImportDialogType::Scene))
		{
			UnregisterLevelEditorWorkspace();
			return false;
		}
		std::string PresentationError;
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetTypePresentation({
				.AssetClassName = DLevel::StaticClass()->GetQualifiedName().ToString(),
				.DisplayName = "Level",
				.Category = Editor::ContentBrowser::EAssetCategory::Level,
				.Icon = Icons::Home,
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterLevelEditorWorkspace();
				return false;
			}
			Integration->TypePresentations.push_back(std::move(Handle));
		}
		return true;
	}

	LEVELEDITOR_API auto FLevelEditorModule::UnregisterLevelEditorWorkspace() -> void
	{
		Integration->TypePresentations.clear();
		Integration->ContentBrowserExtensions.clear();
		WorkspaceRegistration.reset();
		LevelEditorWorkspace.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::OpenDefaultDocument() -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		return Workspace && Workspace->OpenDefaultDocument();
	}

	LEVELEDITOR_API auto FLevelEditorModule::RequestOpenProject(
		std::string ProjectFile) -> bool
	{
		const std::shared_ptr<MLevelEditor> Workspace = LevelEditorWorkspace.lock();
		if (!Workspace) return false;
		Workspace->RequestOpenProject(std::move(ProjectFile));
		return true;
	}

}
