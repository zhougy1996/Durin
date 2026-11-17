#include "ObjectTools.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Editor/AssetEditorTestQueries.h"
#include "Asset/Testing/PackageReloadTesting.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Documents/EditorDocumentManager.h"
#include "MaterialEditorModule.h"
#include "StaticMeshEditorModule.h"
#include "TextureEditorModule.h"
#include "FTextureEditorToolkit.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Texture/VolumeTexture.h"
#include "AssetTools/IAssetTools.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Class.h"
#include <gtest/gtest.h>

namespace Durin::TextureEditor
{
	struct FTextureEditorToolkitTestAccess
	{
		static auto SelectVolume(FAssetEditorToolkit& Toolkit, const std::string& Resource) -> void
		{
			auto& State = static_cast<FTextureEditorToolkit&>(Toolkit).VolumePreviewStates.at(Resource);
			State.Axis = EVolumeTexturePreviewAxis::XZ;
			State.Slice = 1;
			State.Channel = ETexturePreviewChannel::Red;
			State.Zoom = 2.0f;
		}
		static auto VolumeSelection(FAssetEditorToolkit& Toolkit, const std::string& Resource)
		{
			const auto& State = static_cast<FTextureEditorToolkit&>(Toolkit).VolumePreviewStates.at(Resource);
			return std::tuple{State.Axis, State.Slice, State.Mip, State.Channel, State.Zoom};
		}
		static auto Select2D(FAssetEditorToolkit& Toolkit, const std::string& Resource) -> void
		{
			auto& State = static_cast<FTextureEditorToolkit&>(Toolkit).PreviewStates.at(Resource);
			State.bPreviewSource = true;
			State.Zoom = 3.0f;
		}
		static auto Selection2D(FAssetEditorToolkit& Toolkit, const std::string& Resource)
		{
			const auto& State = static_cast<FTextureEditorToolkit&>(Toolkit).PreviewStates.at(Resource);
			return std::pair{State.bPreviewSource, State.Zoom};
		}
	};
}

namespace
{
	using namespace Durin;
	class FAssetEditorFamilyTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			std::string Error;
			ASSERT_TRUE(Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
			Testing::RegisterMountPointForTests("/EditorFamilies/", (Testing::GetTestWorkDirectory() / "Families").generic_string() + "/");
			TextureHarness = std::make_unique<FModuleTestHarness>("TextureEditor"); TextureHarness->Start(TextureModule);
			MaterialHarness = std::make_unique<FModuleTestHarness>("MaterialEditor"); MaterialHarness->Start(MaterialModule);
			MeshHarness = std::make_unique<FModuleTestHarness>("StaticMeshEditor"); MeshHarness->Start(MeshModule);
			Host = NewObject<DEditorEngine>(nullptr, {});
			ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
			AssetEditorSubsystem = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		}
		auto TearDown() -> void override
		{
			(void)FAssetCompilingManager::Get().FinishAllCompilation();
			if (Host) Host->PreExit();
			if (CollectionWorld) { CollectionWorld->CleanupWorld(); MarkObjectHierarchyAsGarbage(CollectionWorld.Get()); CollectionWorld.Reset(); }
			CollectionSubsystemRegistration.reset();
			if (Host) { MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
			if (MeshHarness) MeshHarness->Shutdown();
			if (MaterialHarness) MaterialHarness->Shutdown();
			if (TextureHarness) TextureHarness->Shutdown();
			CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto Open(DObject& Object, EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> bool
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Object.GetObjectPath(), Path)) return {};
			return AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = {{Path, Object.GetClass()->GetQualifiedName().ToString()}}, .OpenMethod = Method});
		}
		template<class T> auto Create(const char* Name) -> T*
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(std::string("/EditorFamilies/") + Name, Path)) return nullptr;
			return NewObject<T>(CreatePackage(Path), Name);
		}
		auto Duplicate(DObject& Source) -> DObject*
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Source.GetObjectPath(), Path)) return nullptr;
			std::string ResultError;
			Durin::FFeedbackContext ResultFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
				if (Level >= Durin::ELogLevel::Error) { if (!ResultError.empty()) ResultError += "\n"; ResultError += Message; }
			});
			const auto Result = ([&] { Durin::FScopedFeedbackContext Scope(*(&ResultFeedback)); auto* Source = Durin::LoadObject(Path.GetAssetPath(), nullptr); std::vector<Durin::DObject*> Copies; Durin::ObjectTools::DuplicateObjects(std::span{&Source, 1}, {}, "/EditorFamilies", false, &Copies); return Copies.empty() ? nullptr : Copies.front(); }());
			return Result && SavePackage(Result->GetPackage()) ? Result : nullptr;
		}
		Tests::FAssetThumbnailFixtureSet Fixtures;
		FTextureEditorModule TextureModule;
		FMaterialEditorModule MaterialModule;
		FStaticMeshEditorModule MeshModule;
		std::unique_ptr<FModuleTestHarness> TextureHarness, MaterialHarness, MeshHarness;
		TStrongObjectPtr<DEditorEngine> Host;
		DAssetEditorSubsystem* AssetEditorSubsystem = nullptr;
		TStrongObjectPtr<DWorld> CollectionWorld;
		std::unique_ptr<FWorldSubsystemRegistration> CollectionSubsystemRegistration;
	};

	TEST_F(FAssetEditorFamilyTests, ReflectedDefinitionsDiscoverAllFamiliesWithoutToolkitBatches)
	{
		for (auto* Class : {DTexture2D::StaticClass(), DVolumeTexture::StaticClass(), DStaticMesh::StaticClass(),
			DMaterial::StaticClass(), DMaterialInstance::StaticClass(), DMaterialFunction::StaticClass(), DMaterialParameterCollection::StaticClass()})
		{
			const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Class);
			ASSERT_NE(nullptr, Definition) << Class->GetName();
			EXPECT_FALSE(Definition->GetAssetDisplayName().empty());
			EXPECT_FALSE(Definition->GetAssetCategories().empty());
		}
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorFamilyTests, MaterialAndInstanceOwnSeparateToolkitsAndSaveThroughTheirEditors)
	{
		for (auto* Source : {static_cast<DObject*>(Fixtures.Material), static_cast<DObject*>(Fixtures.MaterialInstance)})
		{
			auto* Object = Duplicate(*Source);
			ASSERT_NE(nullptr, Object);
			ASSERT_TRUE(Open(*Object));
			auto* EditorInstance = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit);
			ASSERT_NE(nullptr, EditorInstance);
			EXPECT_TRUE(EditorInstance->GetToolkit()->CanSaveActiveDocument());
			EXPECT_TRUE(EditorInstance->GetToolkit()->SaveActiveDocument());
			Object->MarkPackageDirty();
			const auto Document = EditorInstance->GetDocumentManager()->GetDocuments().front();
			ASSERT_TRUE(EditorInstance->GetToolkit()->DiscardDocument(Document)) << EditorInstance->GetToolkit()->GetLastOperationError();
			Object = EditorInstance->GetObjectsToEdit().front();
			EXPECT_FALSE(Open(*Object, EAssetOpenMethod::View));
			EXPECT_EQ(nullptr, AssetEditorSubsystem->OpenAssetEditor(EditorInstance->GetClass(), {
				.OpenArgs = {.AssetEditors = AssetEditorSubsystem, .OpenMethod = EAssetOpenMethod::View, .InstanceRole = "BypassAttempt"}, .Assets = {Object}}));
			EXPECT_TRUE(Open(*Object));
		}
		const auto EditorInstances = Durin::Testing::GetReflectedEditors(AssetEditorSubsystem);
		ASSERT_EQ(2u, EditorInstances.size());
		EXPECT_NE(EditorInstances.front()->GetToolkit(), EditorInstances.back()->GetToolkit());
	}

	TEST_F(FAssetEditorFamilyTests, MixedDirectMeshAndGenericInspectorGroupsRejectBothInputOrders)
	{
		FSoftObjectPath CubePath, MeshPath;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Fixtures.DirectionalCube->GetObjectPath(), CubePath));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Fixtures.StaticMesh->GetObjectPath(), MeshPath));
		std::vector<FAssetOpenTarget> Targets{
			{CubePath, Fixtures.DirectionalCube->GetClass()->GetQualifiedName().ToString()},
			{MeshPath, Fixtures.StaticMesh->GetClass()->GetQualifiedName().ToString()}};
		for (int Order = 0; Order != 2; ++Order)
		{
			const auto Result = AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem, .Assets = Targets,
				.OpenMethod = EAssetOpenMethod::View, .bCreateSingleEditor = true});
			EXPECT_FALSE(Result);
			EXPECT_TRUE(Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).empty());
			EXPECT_TRUE(AssetEditorSubsystem->GetAllOpenEditors().empty());
			std::ranges::reverse(Targets);
		}
	}

	TEST_F(FAssetEditorFamilyTests, DirectMeshDefinitionFocusesNewAndReusedInspectorsExactlyOnce)
	{
		int Opened = 0, Focused = 0;
		auto Subscription = AssetEditorSubsystem->Subscribe([&](const auto& Event) {
			if (Event.Type == EAssetEditorEvent::Opened) ++Opened;
			if (Event.Type == EAssetEditorEvent::Focused) ++Focused;
		});
		ASSERT_TRUE(Open(*Fixtures.StaticMesh));
		EXPECT_EQ(1, Opened); EXPECT_EQ(1, Focused);
		const auto Toolkit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_TRUE(Open(*Fixtures.StaticMesh));
		EXPECT_EQ(1, Opened); EXPECT_EQ(2, Focused);
		EXPECT_EQ(Toolkit, AssetEditorSubsystem->GetActiveAssetToolkit());
		EXPECT_EQ(1u, Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size());
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
	}

	TEST_F(FAssetEditorFamilyTests, MeshIsReadOnlyAndVolumeAndCollectionSupportView)
	{
		ASSERT_TRUE(Open(*Fixtures.StaticMesh));
		const auto Mesh = Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).front();
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		EXPECT_EQ(nullptr, Mesh->GetOwningAssetEditor());
		EXPECT_EQ(Mesh.get(), AssetEditorSubsystem->FindEditorForAsset(Fixtures.StaticMesh, false));
		EXPECT_FALSE(Mesh->CanSaveActiveDocument());
		EXPECT_FALSE(Mesh->SaveActiveDocument());
		EXPECT_FALSE(Mesh->CanUndo());
		EXPECT_TRUE(Mesh->IsReadOnly());
		EXPECT_TRUE(Mesh->GetPresentationRootKey("StaticMeshEditor").starts_with("StaticMeshEditor.Window."));
		const auto MeshToolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Mesh);
		int Mutations = 0;
		auto Tool = MeshToolkit->RegisterUIExtension({.Commands = {{.Id = "MutateMesh", .Label = "Mutate mesh", .Execute = [&] { ++Mutations; }}}});
		ASSERT_TRUE(Tool);
		EXPECT_FALSE(MeshToolkit->InvokeCommand("MutateMesh"));
		EXPECT_EQ(0, Mutations);
		auto* Volume = Create<DVolumeTexture>("Volume");
		auto* Collection = Create<DMaterialParameterCollection>("Collection");
		ASSERT_NE(nullptr, Volume); ASSERT_NE(nullptr, Collection);
		for (auto* Object : {static_cast<DObject*>(Volume), static_cast<DObject*>(Collection)})
		{
			ASSERT_TRUE(Open(*Object, EAssetOpenMethod::View));
			auto* View = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::View);
			ASSERT_NE(nullptr, View);
			EXPECT_FALSE(View->GetToolkit()->SaveActiveDocument());
			ASSERT_TRUE(Open(*Object));
			EXPECT_NE(View, Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit));
		}
	}

	TEST_F(FAssetEditorFamilyTests, FunctionDefinitionOpensThroughTheSameEditorPipeline)
	{
		auto* Function = Create<DMaterialFunction>("Function");
		ASSERT_NE(nullptr, Function);
		ASSERT_TRUE(Open(*Function));
		auto* EditorInstance = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Function, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, EditorInstance);
		EXPECT_EQ("MaterialFunctionEditor", EditorInstance->GetEditorName().ToString());
		EXPECT_TRUE(EditorInstance->GetToolkit()->CanSaveActiveDocument());
		EXPECT_FALSE(Open(*Function, EAssetOpenMethod::View));
		EXPECT_TRUE(EditorInstance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	}

	TEST_F(FAssetEditorFamilyTests, SharedMaterialControllerPreservesFamilyProfilesAndRestoration)
	{
		auto* Material = Duplicate(*Fixtures.Material);
		auto* Function = Create<DMaterialFunction>("ConsolidatedFunction");
		ASSERT_NE(nullptr, Material);
		ASSERT_NE(nullptr, Function);
		ASSERT_TRUE(SavePackage(Function->GetPackage()));
		ASSERT_TRUE(Open(*Material));
		const auto MaterialToolkit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_TRUE(Open(*Function));
		const auto FunctionToolkit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_NE(MaterialToolkit, FunctionToolkit);
		EXPECT_TRUE(typeid(*MaterialToolkit) == typeid(*FunctionToolkit));
		EXPECT_EQ("MaterialEditor", MaterialToolkit->GetToolkitId().GetValue());
		EXPECT_EQ("MaterialFunctionEditor", FunctionToolkit->GetToolkitId().GetValue());
		EXPECT_EQ(4u, MaterialToolkit->GetLayoutVersion());
		EXPECT_EQ(5u, FunctionToolkit->GetLayoutVersion());
		const auto MaterialProfile = MaterialToolkit->GetLayoutProfileKey();
		const auto FunctionProfile = FunctionToolkit->GetLayoutProfileKey();
		EXPECT_NE(MaterialProfile, FunctionProfile);
		Function->MarkPackageDirty();
		const auto Document = FunctionToolkit->GetDocumentManager()->GetDocuments().front();
		// An applied source's disk dirtiness is independent of the isolated draft.
		EXPECT_FALSE(FunctionToolkit->IsDocumentDirty(Document));
		ASSERT_TRUE(FunctionToolkit->SaveDocument(Document));
		EXPECT_FALSE(FunctionToolkit->IsDocumentDirty(Document));
		ASSERT_TRUE(Open(*Material));
		EXPECT_EQ(MaterialToolkit, AssetEditorSubsystem->GetActiveAssetToolkit());
		const auto Snapshot = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Snapshot));
		for (const auto& [Asset, Profile] : std::array{
			std::pair{Material, MaterialProfile}, std::pair{static_cast<DObject*>(Function), FunctionProfile}})
		{
			auto* Restored = Testing::FindReflectedEditor(AssetEditorSubsystem, Asset, EAssetOpenMethod::Edit);
			ASSERT_NE(nullptr, Restored);
			EXPECT_EQ(Profile, Restored->GetToolkit()->GetLayoutProfileKey());
			EXPECT_TRUE(typeid(*FunctionToolkit) == typeid(*Restored->GetToolkit()));
		}
	}

	TEST_F(FAssetEditorFamilyTests, SharedTextureControllerKeepsVolumeAnd2DFamiliesIndependent)
	{
		auto* Texture = Duplicate(*Fixtures.ParentTexture);
		auto* Volume = Create<DVolumeTexture>("ConsolidatedVolume");
		ASSERT_NE(nullptr, Texture);
		ASSERT_NE(nullptr, Volume);
		FVolumeTextureSourceData Source{.Width = 2, .Height = 2, .Depth = 2,
			.Format = EVolumeTextureFormat::R8_UNORM};
		ASSERT_TRUE(Source.SetVoxelBytes(FByteBuffer(8, std::byte{17})));
		auto Prepared = PrepareVolumeTextureSource(Source);
		ASSERT_TRUE(Prepared);
		Volume->SetSource(std::move(*Prepared));
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Volume);
		ASSERT_TRUE(SavePackage(Volume->GetPackage()));
		ASSERT_TRUE(Open(*Texture));
		const auto TextureToolkit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_TRUE(Open(*Volume));
		const auto VolumeToolkit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_NE(TextureToolkit, VolumeToolkit);
		EXPECT_TRUE(typeid(*TextureToolkit) == typeid(*VolumeToolkit));
		EXPECT_EQ("TextureEditor", TextureToolkit->GetToolkitId().GetValue());
		EXPECT_EQ("VolumeTextureEditor", VolumeToolkit->GetToolkitId().GetValue());
		const auto TextureProfile = TextureToolkit->GetLayoutProfileKey();
		const auto VolumeProfile = VolumeToolkit->GetLayoutProfileKey();
		EXPECT_NE(TextureProfile, VolumeProfile);
		using FPreviewAccess = TextureEditor::FTextureEditorToolkitTestAccess;
		const auto TextureResource = Texture->GetObjectPath();
		const auto VolumeResource = Volume->GetObjectPath();
		FPreviewAccess::Select2D(*TextureToolkit, TextureResource);
		FPreviewAccess::SelectVolume(*VolumeToolkit, VolumeResource);
		const auto TextureSelection = FPreviewAccess::Selection2D(*TextureToolkit, TextureResource);
		VolumeToolkit->ResetLayout();
		EXPECT_EQ(TextureSelection, FPreviewAccess::Selection2D(*TextureToolkit, TextureResource));
		EXPECT_EQ((std::tuple{TextureEditor::EVolumeTexturePreviewAxis::XY, 0u, 0u,
			ETexturePreviewChannel::RGBA, 0.0f}), FPreviewAccess::VolumeSelection(*VolumeToolkit, VolumeResource));
		Volume->MarkPackageDirty();
		const auto Document = VolumeToolkit->GetDocumentManager()->GetDocuments().front();
		EXPECT_TRUE(VolumeToolkit->IsDocumentDirty(Document));
		EXPECT_FALSE(TextureToolkit->IsDocumentDirty(TextureToolkit->GetDocumentManager()->GetDocuments().front()));
		ASSERT_TRUE(VolumeToolkit->SaveDocument(Document));
		const auto Snapshot = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Snapshot));
		for (const auto& [Asset, Profile] : std::array{
			std::pair{Texture, TextureProfile}, std::pair{static_cast<DObject*>(Volume), VolumeProfile}})
		{
			auto* Restored = Testing::FindReflectedEditor(AssetEditorSubsystem, Asset, EAssetOpenMethod::Edit);
			ASSERT_NE(nullptr, Restored);
			EXPECT_EQ(Profile, Restored->GetToolkit()->GetLayoutProfileKey());
			EXPECT_TRUE(typeid(*VolumeToolkit) == typeid(*Restored->GetToolkit()));
		}
	}

	TEST_F(FAssetEditorFamilyTests, CollectionDiscardPublishesReplacementRootsAndAssociations)
	{
		auto* Collection = Create<DMaterialParameterCollection>("ReloadCollection");
		ASSERT_NE(nullptr, Collection);
		const FMaterialParameterCollectionDeclaration Declaration{.Id = FGuid::NewGuid(), .Name = FName("Parameter"), .DefaultValue = FVector4(1)};
		ASSERT_TRUE(Collection->SetDeclarations(std::span{&Declaration, 1}));
		ASSERT_TRUE(SavePackage(Collection->GetPackage()));
		CollectionSubsystemRegistration = std::make_unique<FWorldSubsystemRegistration>(FSubsystemDescriptor{.Type = DMaterialParameterCollectionSubsystem::StaticClass()});
		CollectionWorld = NewObject<DWorld>(nullptr, "CollectionReloadWorld");
		ASSERT_NO_THROW(CollectionWorld->InitializeSubsystems());
		auto* WorldState = CollectionWorld->GetSubsystem<DMaterialParameterCollectionSubsystem>();
		ASSERT_NE(nullptr, WorldState);
		ASSERT_TRUE(WorldState->SetValue(*Collection, Declaration.Id, FVector4(7)));
		ASSERT_TRUE(Open(*Collection));
		auto* EditorInstance = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Collection, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, EditorInstance);
		const auto Document = EditorInstance->GetDocumentManager()->GetDocuments().front();
		ASSERT_TRUE(Collection->SetDefaultValue(Declaration.Id, FVector4(3)));
		EXPECT_TRUE(EditorInstance->GetToolkit()->IsDocumentDirty(Document));
		FPackageReloadRequest FailedReload{.Packages = {Collection->GetPackage()}};
		FailedReload.Participants.push_back(EditorInstance->GetDocumentManager()->CreateDocumentReloadParticipant());
		FailedReload.Participants.push_back(CreateTransactorReloadParticipant(*Host->GetTransactor(), FailedReload.Packages));
		{
			Testing::FScopedPackageReloadFault FaultScope([](auto Point, uint64, uint64) {
				return Point == Testing::EPackageReloadFaultPoint::BeforeCommit;
			});
			EXPECT_EQ(EPackageReloadStatus::Failed, ReloadPackages(FailedReload).Wait().Status);
		}
		EXPECT_EQ(Collection, EditorInstance->GetObjectsToEdit().front());
		EXPECT_TRUE(WorldState->SetValue(*Collection, Declaration.Id, FVector4(8)));
		ASSERT_TRUE(EditorInstance->GetToolkit()->DiscardDocument(Document)) << EditorInstance->GetToolkit()->GetLastOperationError();
		const auto Objects = EditorInstance->GetObjectsToEdit();
		ASSERT_EQ(1u, Objects.size());
		EXPECT_NE(Collection, Objects.front());
		EXPECT_EQ(EditorInstance, AssetEditorSubsystem->FindEditorForAsset(Objects.front(), false));
		EXPECT_TRUE(AssetEditorSubsystem->FindEditorsForAsset(Collection).empty());
		auto* Replacement = Cast<DMaterialParameterCollection>(Objects.front());
		ASSERT_NE(nullptr, Replacement);
		FVector4 Override;
		EXPECT_TRUE(WorldState->GetValue(*Replacement, Declaration.Id, Override));
		EXPECT_EQ(FVector4(8, 0, 0, 0), Override);
		EXPECT_TRUE(WorldState->SetValue(*Replacement, Declaration.Id, FVector4(9)));
	}

	TEST_F(FAssetEditorFamilyTests, DirectMeshRelocationRestoreAndUnsupportedReloadPreserveTheInspector)
	{
		auto* Object = Duplicate(*Fixtures.StaticMesh);
		ASSERT_NE(nullptr, Object);
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Object);
		ASSERT_TRUE(Open(*Object));
		const auto Edit = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_TRUE(Edit);
		EXPECT_EQ(nullptr, Edit->GetOwningAssetEditor());
		EXPECT_TRUE(AssetEditorSubsystem->OpenEditorForAsset(Object));
		EXPECT_EQ(Edit, AssetEditorSubsystem->GetActiveAssetToolkit());
		FSoftObjectPath Source;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Source));
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {{Source, Object->GetClass()->GetQualifiedName().ToString()}}, .OpenMethod = EAssetOpenMethod::View,
			.ToolkitMode = EToolkitMode::WorldCentric, .InstanceRole = "Inspection", .bPrimary = false}));
		const auto View = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_NE(Edit, View);
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		for (const auto& Toolkit : {Edit, View})
		{
			EXPECT_TRUE(Toolkit->IsReadOnly());
			EXPECT_FALSE(Toolkit->SaveActiveDocument());
			EXPECT_FALSE(Toolkit->Undo());
		}
		const auto Window = Edit->GetPresentationRootKey("StaticMeshEditor");
		const auto Profile = Edit->GetLayoutProfileKey();
		FPackageReloadRequest Request{.Packages = {Object->GetPackage()}};
		Request.Participants.push_back(Edit->GetDocumentManager()->CreateDocumentReloadParticipant());
		Request.Participants.push_back(View->GetDocumentManager()->CreateDocumentReloadParticipant());
		const auto Reload = ReloadPackages(Request).Wait();
		EXPECT_EQ(EPackageReloadStatus::Failed, Reload.Status);
		EXPECT_EQ(EPackageReloadFailure::Unsupported, Reload.Failure);
		EXPECT_EQ(Edit.get(), AssetEditorSubsystem->FindEditorForAsset(Object, false));
		FPackagePath Destination;
		ASSERT_TRUE(FPackagePath::TryCreate(Source.GetPackagePath().ToString() + "Moved", Destination));
		const auto TargetText = Destination.ToString();
		const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), TargetText.substr(0, TargetText.find_last_of('/')), Destination.GetPackageName()};
		ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
		for (const auto& Toolkit : {Edit, View})
			EXPECT_EQ(Object->GetObjectPath(), Toolkit->GetDocumentManager()->GetDocuments().front().ResourceId);
		EXPECT_EQ(Window, Edit->GetPresentationRootKey("StaticMeshEditor"));
		EXPECT_EQ(Profile, Edit->GetLayoutProfileKey());
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(Edit.get(), AssetEditorSubsystem->FindEditorForAsset(Object, false));
		auto* Second = Duplicate(*Fixtures.StaticMesh);
		ASSERT_NE(nullptr, Second);
		FSoftObjectPath MovedPath, SecondPath;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), MovedPath));
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Second->GetObjectPath(), SecondPath));
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {{MovedPath, Object->GetClass()->GetQualifiedName().ToString()}, {SecondPath, Second->GetClass()->GetQualifiedName().ToString()}},
			.OpenMethod = EAssetOpenMethod::View, .ToolkitMode = EToolkitMode::WorldCentric,
			.InstanceRole = "GroupInspection", .bPrimary = false, .bCreateSingleEditor = true}));
		EXPECT_EQ(2u, AssetEditorSubsystem->GetActiveAssetToolkit()->GetDocumentManager()->GetDocuments().size());
		const auto Group = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_TRUE(AssetEditorSubsystem->OpenAssets({.AssetEditors = AssetEditorSubsystem,
			.Assets = {{SecondPath, Second->GetClass()->GetQualifiedName().ToString()}, {MovedPath, Object->GetClass()->GetQualifiedName().ToString()}},
			.OpenMethod = EAssetOpenMethod::View, .ToolkitMode = EToolkitMode::WorldCentric,
			.InstanceRole = "GroupInspection", .bPrimary = false, .bCreateSingleEditor = true}));
		EXPECT_EQ(Group, AssetEditorSubsystem->GetActiveAssetToolkit());
		const auto Snapshot = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_EQ(3u, Snapshot.size());
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		EXPECT_TRUE(Edit->IsClosed()); EXPECT_TRUE(View->IsClosed());
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Snapshot));
		EXPECT_TRUE(Durin::Testing::GetReflectedEditors(AssetEditorSubsystem).empty());
		ASSERT_EQ(3u, Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem).size());
		for (const auto& Toolkit : Durin::FToolkitManager::Get().GetToolkits(AssetEditorSubsystem))
		{
			EXPECT_EQ(1u, Toolkit->GetObjectsToEdit().size());
			EXPECT_EQ(EAssetEditorOpenLocation::MainWindow, Toolkit->GetOpenLocation());
			EXPECT_TRUE(Toolkit->IsPrimaryEditor());
			EXPECT_TRUE(Toolkit->GetInstanceKey().empty());
			if (Toolkit->GetOpenMethod() == EAssetOpenMethod::Edit)
			{
				EXPECT_EQ(Profile, Toolkit->GetLayoutProfileKey());
				EXPECT_NE(Window, Toolkit->GetPresentationRootKey("StaticMeshEditor"));
			}
		}
	}

	TEST_F(FAssetEditorFamilyTests, MaterialRelocationRekeysWorkingEditorAndRetainsSave)
	{
		auto* Object = Duplicate(*Fixtures.Material);
		ASSERT_NE(nullptr, Object);
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Object);
		ASSERT_TRUE(Open(*Object));
		auto* EditorInstance = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, EditorInstance);
		const auto Window = EditorInstance->GetToolkit()->GetPresentationRootKey("MaterialEditor");
		const auto Profile = EditorInstance->GetToolkit()->GetLayoutProfileKey();
		FSoftObjectPath Source;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Source));
		FPackagePath Destination;
		ASSERT_TRUE(FPackagePath::TryCreate(Source.GetPackagePath().ToString() + "Moved", Destination));
		const auto TargetText = Destination.ToString();
		const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Object), TargetText.substr(0, TargetText.find_last_of('/')), Destination.GetPackageName()};
		ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
		EXPECT_EQ(Object->GetObjectPath(), EditorInstance->GetDocumentManager()->GetDocuments().front().ResourceId);
		EXPECT_EQ(Window, EditorInstance->GetToolkit()->GetPresentationRootKey("MaterialEditor"));
		EXPECT_EQ(Profile, EditorInstance->GetToolkit()->GetLayoutProfileKey());
		EXPECT_TRUE(EditorInstance->GetToolkit()->CanSaveActiveDocument());
		EXPECT_TRUE(EditorInstance->GetToolkit()->SaveActiveDocument());
		const auto Snapshot = AssetEditorSubsystem->CaptureEditorState();
		ASSERT_TRUE(AssetEditorSubsystem->CloseAllAssetEditors());
		ASSERT_TRUE(AssetEditorSubsystem->RestoreEditorState(Snapshot));
		auto* Restored = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Restored);
		EXPECT_EQ(Profile, Restored->GetToolkit()->GetLayoutProfileKey());
		EXPECT_NE(Window, Restored->GetToolkit()->GetPresentationRootKey("MaterialEditor"));
	}

	TEST_F(FAssetEditorFamilyTests, VolumeDiscardReloadsItsSavedSourceThroughTheOwnedEditor)
	{
		auto* Volume = Create<DVolumeTexture>("ReloadVolume");
		ASSERT_NE(nullptr, Volume);
		auto SetSource = [&](std::byte Value) {
			FVolumeTextureSourceData Source{.Width = 2, .Height = 2, .Depth = 2, .Format = EVolumeTextureFormat::R8_UNORM};
			if (!Source.SetVoxelBytes(FByteBuffer(8, Value))) return false;
			auto Prepared = PrepareVolumeTextureSource(Source);
			if (!Prepared) return false;
			Volume->SetSource(std::move(*Prepared));
			return true;
		};
		ASSERT_TRUE(SetSource(std::byte{17}));
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Volume);
		ASSERT_TRUE(SavePackage(Volume->GetPackage()));
		ASSERT_TRUE(Open(*Volume));
		auto* EditorInstance = Durin::Testing::FindReflectedEditor(AssetEditorSubsystem, Volume, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, EditorInstance);
		using FPreviewAccess = TextureEditor::FTextureEditorToolkitTestAccess;
		const auto Resource = Volume->GetObjectPath();
		FPreviewAccess::SelectVolume(*EditorInstance->GetToolkit(), Resource);
		const auto Selection = FPreviewAccess::VolumeSelection(*EditorInstance->GetToolkit(), Resource);
		ASSERT_TRUE(SetSource(std::byte{39}));
		const auto Document = EditorInstance->GetDocumentManager()->GetDocuments().front();
		ASSERT_TRUE(EditorInstance->GetToolkit()->DiscardDocument(Document)) << EditorInstance->GetToolkit()->GetLastOperationError();
		EXPECT_NE(Volume, EditorInstance->GetObjectsToEdit().front());
		EXPECT_EQ(Selection, FPreviewAccess::VolumeSelection(*EditorInstance->GetToolkit(), Resource));
		EXPECT_TRUE(EditorInstance->GetToolkit()->SaveActiveDocument());
	}

	TEST_F(FAssetEditorFamilyTests, RetiredVolumeToolkitDoesNotCancelReopenedEditorsBuild)
	{
		auto* Volume = Create<DVolumeTexture>("ReviewReopenedVolume");
		ASSERT_NE(nullptr, Volume);
		auto SetSource = [&](std::byte Value) {
			FVolumeTextureSourceData Source{.Width = 2, .Height = 2, .Depth = 2, .Format = EVolumeTextureFormat::R8_UNORM};
			if (!Source.SetVoxelBytes(FByteBuffer(8, Value))) return false;
			auto Prepared = PrepareVolumeTextureSource(Source);
			if (!Prepared) return false;
			Volume->SetSource(std::move(*Prepared));
			return true;
		};
		ASSERT_TRUE(SetSource(std::byte{17}));
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Volume);
		ASSERT_TRUE(SavePackage(Volume->GetPackage()));
		ASSERT_TRUE(Open(*Volume));
		auto Retired = AssetEditorSubsystem->GetActiveAssetToolkit();
		FToolkitManager::Get().CloseToolkit(*Retired);
		ASSERT_TRUE(Retired->IsClosed());
		ASSERT_EQ(nullptr, Retired->GetOwningAssetEditor());
		ASSERT_TRUE(Open(*Volume));
		const auto Current = AssetEditorSubsystem->GetActiveAssetToolkit();
		ASSERT_NE(Retired, Current);
		ASSERT_TRUE(SetSource(std::byte{39}));
		Volume->CachePlatformData(true);
		EXPECT_FALSE(Volume->IsAsyncCacheComplete());
		Retired.reset();
		EXPECT_TRUE(Volume->FinishCachePlatformData()) << "Destroying a closed toolkit canceled the new editor's build";
		Volume->GetPackage()->ClearDirty();
	}
}
