#include "LevelEditorModule.h"
#include "LevelAssetEditor.h"
#include "DObject/DObjectArray.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitHost.h"
#include "Editor/LevelEditorSubsystem.h"
#include "Tools/PIEToolset.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Documents/EditorDocumentManager.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Settings/ProjectDefaultLevelReferenceStore.h"
#include "Engine/Level.h"
#include "Actors/PlayerStart.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/VolumetricCloudActor.h"
#include "Customizations/ActorComponentDetails.h"
#include "Actors/SplineMeshActor.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Widgets/MLevelEditor.h"
#include "Customizations/CameraEditorCustomizations.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
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
		auto CreateLevelAsset(const FTopLevelAssetPath& AssetPath, std::string& OutError) -> bool
		{
			const FAssetToolsResult Created = IAssetTools::Get().CreateAsset(
				AssetPath, DLevel::StaticClass());
			if (!Created)
			{
				OutError = Created.Message;
				return false;
			}
			const FAssetWriteResult Result = SavePackage(Created.Package);
			if (!Result)
			{
				OutError = Result.Message;
				if (!IAssetTools::Get().DiscardPackage(Created.Package))
					OutError += " The unsaved level package could not be discarded.";
				return false;
			}
			return true;
		}

	}

	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	struct FLevelEditorModule::FIntegrationState
	{
		FName Provider;
		std::unique_ptr<FEditorSubsystemRegistration> Levels;
		std::unique_ptr<FNativeToolset> PIE;
		FModularFeatureRegistration PIERegistration;
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
		Integration->Provider = FModuleStartup::GetModuleName();
		ProjectDefaultLevelReferenceStore =
			std::make_unique<FProjectDefaultLevelReferenceStore>(
				[this](const FPackagePath& Path) {
					for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
						if (const auto* Session = Cast<DLevelAssetEditor>(Object); Session && !Session->IsClosed())
							if (auto Workspace = std::dynamic_pointer_cast<MLevelEditor>(Session->GetToolkit())) Workspace->ApplyFixedUpDefaultLevelPath(Path);
				});
		ProjectDefaultLevelReferenceStoreHandle =
			RegisterAssetReferenceStore(
				ProjectDefaultLevelReferenceStore.get());

		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(AStaticMeshActor::StaticClass(),
			CreateStaticMeshActorDetailsCustomization()));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ACameraActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<ACameraActor>(Actor)->GetCameraComponent(); })));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ADirectionalLightActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<ADirectionalLightActor>(Actor)->GetLightComponent(); })));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(AVolumetricCloudActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<AVolumetricCloudActor>(Actor)->GetVolumetricCloudComponent(); })));

		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DSkyLightComponent::StaticClass(),
			CreateSkyLightDetailsCustomization()));

		CustomizationHandles.push_back(Registry.RegisterActorVisualizer(APlayerStart::StaticClass(), CreatePlayerStartActorVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer()));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DCameraComponent::StaticClass(), CreateCameraDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer()));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization()));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ASplineMeshActor::StaticClass(), CreateSplineMeshActorDetailsCustomization()));
		SplineEditModeHandle = RegisterSplineViewportEditMode();
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DStaticMeshComponent::StaticClass(), CreateStaticMeshComponentDetailsCustomization()));
		DetailCustomizationHandles.push_back(Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DVolumetricCloudComponent::StaticClass(), CreateVolumetricCloudDetailsCustomization()));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
		checkf(std::ranges::all_of(DetailCustomizationHandles, [](Editor::FDetailCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in detail customizations must register exactly once");
		Integration->Levels = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = DLevelEditorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		Integration->PIE = CreatePIEToolset();
		Integration->PIERegistration = FModularFeatureRegistry::Get().RegisterFeature(*Integration->PIE);
		require(RegisterContentBrowserExtensions());
	}

	LEVELEDITOR_API auto FLevelEditorModule::CanShutdownModule() const -> bool
	{
		return FEditorSubsystemCollection::CanSynchronizeActiveEditor(Integration->Provider);
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		require(CanShutdownModule());
		Integration->ContentBrowserExtensions.clear();
		Integration->PIERegistration.Reset();
		Integration->PIE.reset();
		Integration->Levels.reset();
		FEditorSubsystemCollection::SynchronizeActiveEditor();
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (auto* Session = Cast<DLevelAssetEditor>(Object); Session && !Session->IsClosed())
				if (auto* Service = Session->GetAssetEditorSubsystem()) Service->RetireAssetEditor(*Session);
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
		for (auto It = DetailCustomizationHandles.rbegin(); It != DetailCustomizationHandles.rend(); ++It) Editor::FDetailCustomizationRegistry::Get().Unregister(*It);
		DetailCustomizationHandles.clear();
	}

	LEVELEDITOR_API auto FLevelEditorModule::InitializeLevelEditor(
		DAssetEditorSubsystem& AssetEditors, FContentBrowserCallbacks ContentBrowserCallbacks,
		std::function<void()> OpenProjectBrowser) -> bool
	{
		auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>();
		check(Levels && !Levels->GetHostContext());
		auto Context = std::make_shared<FLevelEditorHostContext>();
		Context->Settings = std::make_shared<FLevelEditorSessionSettings>();
		Context->Settings->Load();
		Context->Browser = std::move(ContentBrowserCallbacks);
		Context->OpenProjectBrowser = std::move(OpenProjectBrowser);
		Levels->SetHostContext(Context);
		if (!GetOrCreateLevelEditor(AssetEditors)) { Levels->SetHostContext({}); return false; }
		return true;
	}

	auto FLevelEditorModule::RegisterContentBrowserExtensions() -> bool
	{
		{
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterAssetCreation({
				.Id = "level.create-level",
				.Label = "Level",
				.DefaultName = "NewLevel",
				.Order = 100,
				.Create = CreateLevelAsset,
				}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser Level creation: {}", Error);
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
		}
		std::string Error;
		auto Handle = Editor::ContentBrowser::RegisterExtension({
			.Id = "level.import-scene",
			.Label = "Scene Source (FBX)...",
			.Category = Editor::ContentBrowser::EExtensionCategory::Import,
			.Order = 300,
			.Mutation = Editor::ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [](const auto& Invocation) {
				auto* Service = Invocation.AssetEditors.Get();
				if (!Service) return;
				for (const auto& Session : Service->GetOwnedAssetEditors())
				if (auto Toolkit = std::dynamic_pointer_cast<MLevelEditor>(Session->GetToolkit()))
					Toolkit->RequestContentBrowserImport(Invocation.Context.VirtualDirectory, Editor::Level::EImportDialogType::Scene);
			},
			.DrawHostPresentation = [](const Editor::ContentBrowser::FExtensionHost& Host, bool bAllowed) {
				auto* Service = Host.AssetEditors.Get();
				if (!Service) return;
				for (const auto& Session : Service->GetOwnedAssetEditors())
				if (auto Toolkit = std::dynamic_pointer_cast<MLevelEditor>(Session->GetToolkit()))
					Toolkit->DrawContentBrowserImport(Editor::Level::EImportDialogType::Scene, bAllowed);
			},
			}, Error);
		if (!Handle.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser import: {}", Error);
			return false;
		}
		Integration->ContentBrowserExtensions.push_back(std::move(Handle));
		return true;
	}

	auto FLevelEditorModule::GetOrCreateLevelEditor(DAssetEditorSubsystem& AssetEditors) -> DAssetEditor*
	{
		for (const auto& Session : AssetEditors.GetOwnedAssetEditors())
			if (auto* Level = Cast<DLevelAssetEditor>(Session.Get())) { Level->FocusWindow(); return Level; }
		auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>();
		if (!Levels || !Levels->GetHostContext()) return nullptr;
		auto* Session = NewObject<DLevelAssetEditor>(AssetEditors.GetEditor(), {});
		Session->ConfigureHost(Levels->GetHostContext());
		return AssetEditors.InitializeAssetEditor(*Session, {
			.OpenArgs = {.AssetEditors = &AssetEditors, .ToolkitMode = EToolkitMode::WorldCentric},
			.InitialDocuments = {{.ToolkitId = Workspace::Type,
				.DocumentKey = "LevelEditor", .Label = "Level Editor"}}});
	}

	LEVELEDITOR_API auto FLevelEditorModule::ReleaseLevelEditor(DAssetEditorSubsystem& AssetEditors) -> void
	{
		for (const auto& Session : AssetEditors.GetOwnedAssetEditors())
			if (Cast<DLevelAssetEditor>(Session.Get())) AssetEditors.RetireAssetEditor(*Session);
		if (auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>()) Levels->SetHostContext({});
	}

	LEVELEDITOR_API auto FLevelEditorModule::RequestOpenProject(
		DAssetEditorSubsystem& AssetEditors, std::string ProjectFile) -> bool
	{
		for (const auto& Session : AssetEditors.GetOwnedAssetEditors())
			if (const auto Workspace = std::dynamic_pointer_cast<MLevelEditor>(Session->GetToolkit()))
			{ Workspace->RequestOpenProject(std::move(ProjectFile)); return true; }
		return false;
	}

}
