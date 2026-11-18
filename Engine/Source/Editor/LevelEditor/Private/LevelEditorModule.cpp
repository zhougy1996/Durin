#include "PackageTools.h"
#include "Misc/FeedbackContext.h"
#include "LevelEditorModule.h"
#include "Toolkits/ToolkitManager.h"
#include "LevelAssetEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitHost.h"
#include "LevelEditorSubsystem.h"
#include "Tools/PIEToolset.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Documents/EditorDocumentManager.h"
#include "Editor/EditorEngine.h"
#include "DObject/ObjectLifecycle.h"
#include "Transactions/Transaction.h"
#include "Settings/LevelEditorWorkspaceSettings.h"
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
#include "FLevelEditorToolkit.h"
#include "Viewport/SelectViewportEditMode.h"
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
	using namespace LevelEditor;
	namespace
	{
		auto CreateLevelAsset(const FTopLevelAssetPath& AssetPath, std::string& OutError) -> bool
		{
			std::string CreatedError;
			FFeedbackContext CreatedFeedback({}, [&](ELogLevel Level, std::string_view Message) {
				if (Level >= ELogLevel::Error)
				{
					if (!CreatedError.empty()) CreatedError += "\n";
					CreatedError += Message;
				}
			});
			Durin::FScopedFeedbackContext CreationFeedbackScope(CreatedFeedback);
			const auto Created = Durin::IAssetTools::Get().CreateAsset((AssetPath).GetAssetName(), (AssetPath).GetPackagePath().ToString().substr(0, (AssetPath).GetPackagePath().ToString().find_last_of('/')), DLevel::StaticClass(), nullptr);
			if (!Created)
			{
				OutError = CreatedError;
				return false;
			}
			FAssetWriteResult Result;
			if (!SavePackage(Created->GetPackage(), {}, &Result))
			{
				OutError = (Result.Error ? Result.Error->ToString() : std::string{});
				if (!([&] { std::string Error; return Durin::DPackageTools::UnloadPackages(std::array{Created->GetPackage()}, Error, true) && Error.empty(); }()))
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
		FLevelViewportEditModeHandle SelectMode;
		std::unique_ptr<FEditorSubsystemRegistration> Levels;
		std::unique_ptr<FNativeToolset> PIE;
		FModularFeatureRegistration PIERegistration;
		std::vector<ContentBrowser::FScopedExtensionRegistration>
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
					for (const auto& Toolkit : FToolkitManager::Get().GetToolkits())
						if (!Toolkit->IsClosed())
							if (auto Workspace = std::dynamic_pointer_cast<FLevelEditorToolkit>(Toolkit)) Workspace->ApplyFixedUpDefaultLevelPath(Path);
				});
		ProjectDefaultLevelReferenceStoreHandle =
			RegisterAssetReferenceStore(
				ProjectDefaultLevelReferenceStore.get());

		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(AStaticMeshActor::StaticClass(),
			CreateStaticMeshActorDetailsCustomization()));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ACameraActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<ACameraActor>(Actor)->GetCameraComponent(); })));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ADirectionalLightActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<ADirectionalLightActor>(Actor)->GetLightComponent(); })));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(AVolumetricCloudActor::StaticClass(),
			CreateActorComponentDetailsCustomization([](AActor* Actor) -> DActorComponent* { return Cast<AVolumetricCloudActor>(Actor)->GetVolumetricCloudComponent(); })));

		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DSkyLightComponent::StaticClass(),
			CreateSkyLightDetailsCustomization()));

		CustomizationHandles.push_back(Registry.RegisterActorVisualizer(APlayerStart::StaticClass(), CreatePlayerStartActorVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DCameraComponent::StaticClass(), CreateCameraComponentVisualizer()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DDirectionalLightComponent::StaticClass(), CreateDirectionalLightComponentVisualizer()));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DCameraComponent::StaticClass(), CreateCameraDetailsCustomization()));
		CustomizationHandles.push_back(Registry.RegisterComponentVisualizer(DSplineComponent::StaticClass(), CreateSplineComponentVisualizer()));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DSplineComponent::StaticClass(), CreateSplineDetailsCustomization()));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(ASplineMeshActor::StaticClass(), CreateSplineMeshActorDetailsCustomization()));
		Integration->SelectMode = RegisterSelectViewportEditMode();
		SplineEditModeHandle = RegisterSplineViewportEditMode();
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DStaticMeshComponent::StaticClass(), CreateStaticMeshComponentDetailsCustomization()));
		DetailCustomizationHandles.push_back(FDetailCustomizationRegistry::Get().RegisterDetailCustomization(DVolumetricCloudComponent::StaticClass(), CreateVolumetricCloudDetailsCustomization()));
		checkf(std::ranges::all_of(CustomizationHandles, [](FLevelEditorCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in customizations must register exactly once");
		checkf(std::ranges::all_of(DetailCustomizationHandles, [](FDetailCustomizationHandle Handle) { return static_cast<bool>(Handle); }), "LevelEditor built-in detail customizations must register exactly once");
		Integration->Levels = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = DLevelEditorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		Integration->PIE = CreatePIEToolset();
		Integration->PIERegistration = FModularFeatureRegistry::Get().RegisterFeature(*Integration->PIE);
		require(RegisterContentBrowserExtensions());
	}

	LEVELEDITOR_API auto FLevelEditorModule::CanShutdownModule() const -> bool
	{
		return !IsGarbageCollecting() && (!GEditor || (!GEditor->IsShutdownRequested() && !GEditor->AreEditorSubsystemsUpdating()
			&& (!GEditor->GetTransactor() || GEditor->GetTransactor()->CanTransact())));
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		Integration->ContentBrowserExtensions.clear();
		Integration->PIERegistration.Reset();
		Integration->PIE.reset();
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits())
			if (Cast<DLevelAssetEditor>(Toolkit->GetOwningAssetEditor())) FToolkitManager::Get().CloseToolkit(*Toolkit);
		if (GEditor)
		{
			const auto Discarded = GEditor->GetTransactor()->DiscardCustomChangesByModule("LevelEditor");
			requiref(Discarded,
				"LevelEditor cannot retire while one of its custom changes is active");
		}
		if (GEditor) GEditor->RemoveEditorSubsystems(Integration->Provider);
		Integration->Levels.reset();
		UnregisterAssetReferenceStore(
			ProjectDefaultLevelReferenceStoreHandle);
		ProjectDefaultLevelReferenceStoreHandle = 0;
		ProjectDefaultLevelReferenceStore.reset();
		auto& Registry = FLevelEditorCustomizationRegistry::Get();
		if (SplineEditModeHandle) FLevelViewportEditModeRegistry::Get().Unregister(SplineEditModeHandle);
		SplineEditModeHandle = {};
		FLevelViewportEditModeRegistry::Get().Unregister(Integration->SelectMode);
		Integration->SelectMode = {};
		for (auto It = CustomizationHandles.rbegin(); It != CustomizationHandles.rend(); ++It) Registry.Unregister(*It);
		CustomizationHandles.clear();
		for (auto It = DetailCustomizationHandles.rbegin(); It != DetailCustomizationHandles.rend(); ++It) FDetailCustomizationRegistry::Get().Unregister(*It);
		DetailCustomizationHandles.clear();
	}

	LEVELEDITOR_API auto FLevelEditorModule::InitializeLevelEditor(
		DAssetEditorSubsystem& AssetEditors, FContentBrowserCallbacks ContentBrowserCallbacks,
		std::function<void()> OpenProjectBrowser) -> bool
	{
		auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>();
		check(Levels && !Levels->GetHostContext());
		auto Context = std::make_shared<FLevelEditorHostContext>();
		Context->Settings = std::make_shared<FLevelEditorWorkspaceSettings>();
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
			auto Handle = ContentBrowser::RegisterAssetCreation({
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
		auto Handle = ContentBrowser::RegisterExtension({
			.Id = "level.import-scene",
			.Label = "Scene Source (FBX)...",
			.Category = ContentBrowser::EExtensionCategory::Import,
			.Order = 300,
			.Mutation = ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [](const auto& Invocation) {
				auto* AssetEditorSubsystem = Invocation.Host.AssetEditors.Get();
				if (!AssetEditorSubsystem) return;
				for (const auto& Entry : FToolkitManager::Get().GetToolkits(AssetEditorSubsystem))
				if (auto Toolkit = std::dynamic_pointer_cast<FLevelEditorToolkit>(Entry))
					Toolkit->RequestContentBrowserImport(Invocation.Context.VirtualDirectory, LevelEditor::EImportDialogType::Scene);
			},
			.DrawHostPresentation = [](const ContentBrowser::FExtensionHost& Host, bool bAllowed) {
				auto* AssetEditorSubsystem = Host.AssetEditors.Get();
				if (!AssetEditorSubsystem) return;
				for (const auto& Entry : FToolkitManager::Get().GetToolkits(AssetEditorSubsystem))
				if (auto Toolkit = std::dynamic_pointer_cast<FLevelEditorToolkit>(Entry))
					Toolkit->DrawContentBrowserImport(LevelEditor::EImportDialogType::Scene, bAllowed);
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
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(&AssetEditors))
			if (auto* Level = Cast<DLevelAssetEditor>(Toolkit->GetOwningAssetEditor()))
			{
				if (Level->GetDocumentManager()->GetDocuments().empty())
					Level->GetDocumentManager()->OpenDocument({.ToolkitId = Workspace::Type,
						.DocumentKey = "LevelEditor", .Label = "Level Editor"});
				Level->FocusWindow();
				return Level;
			}
		auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>();
		if (!Levels || !Levels->GetHostContext()) return nullptr;
		auto* Editor = NewObject<DLevelAssetEditor>(AssetEditors.GetEditor(), {});
		Editor->ConfigureHost(Levels->GetHostContext());
		return AssetEditors.InitializeAssetEditor(*Editor, {
			.OpenArgs = {.AssetEditors = &AssetEditors, .ToolkitMode = EToolkitMode::WorldCentric},
			.InitialDocuments = {{.ToolkitId = Workspace::Type,
				.DocumentKey = "LevelEditor", .Label = "Level Editor"}}});
	}

	LEVELEDITOR_API auto FLevelEditorModule::ReleaseLevelEditor(DAssetEditorSubsystem& AssetEditors) -> void
	{
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(&AssetEditors))
			if (Cast<DLevelAssetEditor>(Toolkit->GetOwningAssetEditor())) FToolkitManager::Get().CloseToolkit(*Toolkit);
		if (auto* Levels = AssetEditors.GetEditor()->GetEditorSubsystem<DLevelEditorSubsystem>()) Levels->SetHostContext({});
	}

	LEVELEDITOR_API auto FLevelEditorModule::RequestOpenProject(
		DAssetEditorSubsystem& AssetEditors, std::string ProjectFile) -> bool
	{
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(&AssetEditors))
			if (const auto Workspace = std::dynamic_pointer_cast<FLevelEditorToolkit>(Toolkit))
			{ Workspace->RequestOpenProject(std::move(ProjectFile)); return true; }
		return false;
	}

}
