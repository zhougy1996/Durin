#include "Thumbnail/AssetThumbnailTestFixtures.h"
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
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Texture/VolumeTexture.h"
#include "AssetTools/IAssetTools.h"
#include "AssetTools/Relocation.h"
#include "DObject/Class.h"
#include <gtest/gtest.h>

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
			Testing::RegisterMountPointForTests("/FamilySessions/", (Testing::GetTestWorkDirectory() / "Families").generic_string() + "/");
			TextureHarness = std::make_unique<FModuleTestHarness>("TextureEditor"); TextureHarness->Start(TextureModule);
			MaterialHarness = std::make_unique<FModuleTestHarness>("MaterialEditor"); MaterialHarness->Start(MaterialModule);
			MeshHarness = std::make_unique<FModuleTestHarness>("StaticMeshEditor"); MeshHarness->Start(MeshModule);
			Host = NewObject<DEditorEngine>(nullptr, {});
			ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
			Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		}
		auto TearDown() -> void override
		{
			(void)FAssetCompilingManager::Get().FinishAllCompilation();
			if (Service) Service->ShutdownAssetEditorSessions();
			if (CollectionWorld) { CollectionWorld->Shutdown(); MarkObjectHierarchyAsGarbage(CollectionWorld.Get()); CollectionWorld.Reset(); }
			CollectionSubsystemRegistration.reset();
			if (Host) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
			if (MeshHarness) MeshHarness->Shutdown();
			if (MaterialHarness) MaterialHarness->Shutdown();
			if (TextureHarness) TextureHarness->Shutdown();
			CollectGarbage();
		}
		auto Open(DObject& Object, EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> FAssetOpenResult
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Object.GetObjectPath(), Path)) return {};
			return Service->OpenAssets({.AssetEditors = Service, .Assets = {{Path, Object.GetClass()->GetQualifiedName().ToString()}}, .OpenMethod = Method});
		}
		template<class T> auto Create(const char* Name) -> T*
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(std::string("/FamilySessions/") + Name, Path)) return nullptr;
			return NewObject<T>(CreatePackage(Path), Name);
		}
		auto Duplicate(DObject& Source) -> DObject*
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Source.GetObjectPath(), Path)) return nullptr;
			const auto Result = IAssetTools::Get().DuplicateAsset({.SourcePath = Path.GetAssetPath(), .DestinationDirectory = "/FamilySessions",
				.ResolvePhysicalPackagePath = [](const FPackagePath& Package) {
					const auto Resolved = FMountPaths::ResolveAssetPath(Package.GetView(), EMountPathExistence::AllowMissing);
					return Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{};
				}});
			return Result ? Result.Asset : nullptr;
		}
		Tests::FAssetThumbnailFixtureSet Fixtures;
		FTextureEditorModule TextureModule;
		FMaterialEditorModule MaterialModule;
		FStaticMeshEditorModule MeshModule;
		std::unique_ptr<FModuleTestHarness> TextureHarness, MaterialHarness, MeshHarness;
		TStrongObjectPtr<DEditorEngine> Host;
		DAssetEditorSubsystem* Service = nullptr;
		TStrongObjectPtr<DWorld> CollectionWorld;
		std::unique_ptr<FWorldSubsystemRegistration> CollectionSubsystemRegistration;
	};

	TEST_F(FAssetEditorFamilyTests, ReflectedDefinitionsDiscoverAllFamiliesWithoutToolkitBatches)
	{
		for (auto* Class : {DTexture2D::StaticClass(), DVolumeTexture::StaticClass(), DStaticMesh::StaticClass(),
			DMaterial::StaticClass(), DMaterialInstance::StaticClass(), DMaterialFunction::StaticClass(), DMaterialParameterCollection::StaticClass()})
		{
			const auto* Definition = FAssetDefinitionRegistry::Get().FindDefinition(Class->GetQualifiedName().ToString());
			ASSERT_NE(nullptr, Definition) << Class->GetName();
			EXPECT_FALSE(Definition->GetAssetDisplayName().empty());
			EXPECT_FALSE(Definition->GetAssetCategories().empty());
		}
		EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
	}

	TEST_F(FAssetEditorFamilyTests, MaterialAndInstanceOwnSeparateToolkitsAndSaveThroughTheirSessions)
	{
		for (auto* Source : {static_cast<DObject*>(Fixtures.Material), static_cast<DObject*>(Fixtures.MaterialInstance)})
		{
			auto* Object = Duplicate(*Source);
			ASSERT_NE(nullptr, Object);
			ASSERT_EQ(EAssetOpenStatus::Completed, Open(*Object).Items.front().Status);
			auto* Session = Service->FindAssetEditor(Object, EAssetOpenMethod::Edit);
			ASSERT_NE(nullptr, Session);
			EXPECT_TRUE(Session->GetToolkit()->CanSaveActiveDocument());
			EXPECT_TRUE(Session->GetToolkit()->SaveActiveDocument());
			Object->MarkPackageDirty();
			const auto Document = Session->GetDocumentManager()->GetDocuments().front();
			ASSERT_TRUE(Session->GetToolkit()->DiscardDocument(Document)) << Session->GetToolkit()->GetLastOperationError();
			Object = Session->GetObjectsToEdit().front();
			EXPECT_EQ(EAssetOpenStatus::Rejected, Open(*Object, EAssetOpenMethod::View).Items.front().Status);
			EXPECT_EQ(nullptr, Service->OpenAssetEditor(Session->GetClass(), {
				.OpenArgs = {.AssetEditors = Service, .OpenMethod = EAssetOpenMethod::View}, .Assets = {Object}, .InstanceKey = "BypassAttempt"}));
			EXPECT_EQ(EAssetOpenStatus::Completed, Open(*Object).Items.front().Status);
		}
		const auto Sessions = Service->GetOwnedAssetEditors();
		ASSERT_EQ(2u, Sessions.size());
		EXPECT_NE(Sessions.front()->GetToolkit(), Sessions.back()->GetToolkit());
	}

	TEST_F(FAssetEditorFamilyTests, MeshIsReadOnlyAndVolumeAndCollectionSupportView)
	{
		ASSERT_EQ(EAssetOpenStatus::Completed, Open(*Fixtures.StaticMesh).Items.front().Status);
		const auto Mesh = Service->GetOwnedAssetEditors().front();
		EXPECT_FALSE(Mesh->GetToolkit()->CanSaveActiveDocument());
		EXPECT_FALSE(Mesh->GetToolkit()->SaveActiveDocument());
		EXPECT_FALSE(Mesh->GetToolkit()->CanUndo());
		EXPECT_TRUE(Mesh->GetToolkit()->IsReadOnly());
		EXPECT_TRUE(Mesh->GetToolkit()->GetPresentationRootKey("StaticMeshEditor").starts_with("StaticMeshEditor.Window."));
		const auto MeshToolkit = std::dynamic_pointer_cast<FBaseAssetToolkit>(Mesh->GetToolkit());
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
			ASSERT_EQ(EAssetOpenStatus::Completed, Open(*Object, EAssetOpenMethod::View).Items.front().Status);
			auto* View = Service->FindAssetEditor(Object, EAssetOpenMethod::View);
			ASSERT_NE(nullptr, View);
			EXPECT_FALSE(View->GetToolkit()->SaveActiveDocument());
			ASSERT_EQ(EAssetOpenStatus::Completed, Open(*Object).Items.front().Status);
			EXPECT_NE(View, Service->FindAssetEditor(Object, EAssetOpenMethod::Edit));
		}
	}

	TEST_F(FAssetEditorFamilyTests, FunctionDefinitionOpensThroughTheSameSessionPipeline)
	{
		auto* Function = Create<DMaterialFunction>("Function");
		ASSERT_NE(nullptr, Function);
		ASSERT_EQ(EAssetOpenStatus::Completed, Open(*Function).Items.front().Status);
		auto* Session = Service->FindAssetEditor(Function, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Session);
		EXPECT_EQ("MaterialFunctionEditor", Session->GetEditorName().ToString());
		EXPECT_TRUE(Session->GetToolkit()->CanSaveActiveDocument());
		EXPECT_EQ(EAssetOpenStatus::Rejected, Open(*Function, EAssetOpenMethod::View).Items.front().Status);
		EXPECT_TRUE(Session->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed));
	}

	TEST_F(FAssetEditorFamilyTests, CollectionDiscardPublishesReplacementRootsAndAssociations)
	{
		auto* Collection = Create<DMaterialParameterCollection>("ReloadCollection");
		ASSERT_NE(nullptr, Collection);
		const FMaterialParameterCollectionDeclaration Declaration{.Id = FGuid::NewGuid(), .Name = FName("Parameter"), .DefaultValue = FVector4(1)};
		ASSERT_TRUE(Collection->SetDeclarations(std::span{&Declaration, 1}));
		ASSERT_TRUE(SavePackage(Collection->GetPackage()));
		CollectionSubsystemRegistration = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{.Type = DMaterialParameterCollectionSubsystem::StaticClass()});
		CollectionWorld = NewObject<DWorld>(nullptr, "CollectionReloadWorld");
		ASSERT_TRUE(CollectionWorld->InitializeSubsystems());
		auto* WorldState = CollectionWorld->GetSubsystem<DMaterialParameterCollectionSubsystem>();
		ASSERT_NE(nullptr, WorldState);
		ASSERT_TRUE(WorldState->SetValue(*Collection, Declaration.Id, FVector4(7)));
		ASSERT_TRUE(Open(*Collection).IsAccepted());
		auto* Session = Service->FindAssetEditor(Collection, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Session);
		const auto Document = Session->GetDocumentManager()->GetDocuments().front();
		ASSERT_TRUE(Collection->SetDefaultValue(Declaration.Id, FVector4(3)));
		EXPECT_TRUE(Session->GetToolkit()->IsDocumentDirty(Document));
		FPackageReloadRequest FailedReload{.Packages = {Collection->GetPackage()}};
		FailedReload.Participants.push_back(Session->GetDocumentManager()->CreateDocumentReloadParticipant());
		FailedReload.Participants.push_back(CreateTransactorReloadParticipant(*Host->GetTransactor(), FailedReload.Packages));
		{
			Testing::FScopedPackageReloadFault FaultScope([](auto Point, uint64, uint64) {
				return Point == Testing::EPackageReloadFaultPoint::BeforeCommit;
			});
			EXPECT_EQ(EPackageReloadStatus::Failed, ReloadPackages(FailedReload).Wait().Status);
		}
		EXPECT_EQ(Collection, Session->GetObjectsToEdit().front());
		EXPECT_TRUE(WorldState->SetValue(*Collection, Declaration.Id, FVector4(8)));
		ASSERT_TRUE(Session->GetToolkit()->DiscardDocument(Document)) << Session->GetToolkit()->GetLastOperationError();
		const auto Objects = Session->GetObjectsToEdit();
		ASSERT_EQ(1u, Objects.size());
		EXPECT_NE(Collection, Objects.front());
		EXPECT_EQ(Session, Service->FindEditorForAsset(Objects.front(), false));
		EXPECT_TRUE(Service->FindEditorsForAsset(Collection).empty());
		auto* Replacement = Cast<DMaterialParameterCollection>(Objects.front());
		ASSERT_NE(nullptr, Replacement);
		FVector4 Override;
		EXPECT_TRUE(WorldState->GetValue(*Replacement, Declaration.Id, Override));
		EXPECT_EQ(FVector4(8, 0, 0, 0), Override);
		EXPECT_TRUE(WorldState->SetValue(*Replacement, Declaration.Id, FVector4(9)));
	}

	TEST_F(FAssetEditorFamilyTests, MaterialRelocationRekeysWorkingSessionAndRetainsSave)
	{
		auto* Object = Duplicate(*Fixtures.Material);
		ASSERT_NE(nullptr, Object);
		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Object);
		ASSERT_TRUE(Open(*Object).IsAccepted());
		auto* Session = Service->FindAssetEditor(Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Session);
		const auto Window = Session->GetToolkit()->GetPresentationRootKey("MaterialEditor");
		const auto Profile = Session->GetToolkit()->GetLayoutProfileKey();
		FSoftObjectPath Source;
		ASSERT_TRUE(FSoftObjectPath::TryCreate(Object->GetObjectPath(), Source));
		FPackagePath Destination;
		ASSERT_TRUE(FPackagePath::TryCreate(Source.GetPackagePath().ToString() + "Moved", Destination));
		const FAssetRelocationMapping Mapping{Source.GetPackagePath(), Destination};
		ASSERT_TRUE(RelocateAssets(std::span{&Mapping, 1}).Result);
		EXPECT_EQ(Object->GetObjectPath(), Session->GetDocumentManager()->GetDocuments().front().ResourceId);
		EXPECT_EQ(Window, Session->GetToolkit()->GetPresentationRootKey("MaterialEditor"));
		EXPECT_EQ(Profile, Session->GetToolkit()->GetLayoutProfileKey());
		EXPECT_TRUE(Session->GetToolkit()->CanSaveActiveDocument());
		EXPECT_TRUE(Session->GetToolkit()->SaveActiveDocument());
		const auto Snapshot = Service->CaptureSessions();
		ASSERT_TRUE(Service->CloseAllAssetEditors());
		ASSERT_TRUE(Service->RestoreSessions(Snapshot).IsAccepted());
		auto* Restored = Service->FindAssetEditor(Object, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Restored);
		EXPECT_EQ(Profile, Restored->GetToolkit()->GetLayoutProfileKey());
		EXPECT_NE(Window, Restored->GetToolkit()->GetPresentationRootKey("MaterialEditor"));
	}

	TEST_F(FAssetEditorFamilyTests, VolumeDiscardReloadsItsSavedSourceThroughTheOwnedSession)
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
		ASSERT_TRUE(Open(*Volume).IsAccepted());
		auto* Session = Service->FindAssetEditor(Volume, EAssetOpenMethod::Edit);
		ASSERT_NE(nullptr, Session);
		ASSERT_TRUE(SetSource(std::byte{39}));
		const auto Document = Session->GetDocumentManager()->GetDocuments().front();
		ASSERT_TRUE(Session->GetToolkit()->DiscardDocument(Document)) << Session->GetToolkit()->GetLastOperationError();
		EXPECT_NE(Volume, Session->GetObjectsToEdit().front());
		EXPECT_TRUE(Session->GetToolkit()->SaveActiveDocument());
	}
}
