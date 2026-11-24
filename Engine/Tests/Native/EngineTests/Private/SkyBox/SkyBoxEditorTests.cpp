#include "NativeAssetRuntimeTestSupport.h"
#include "Misc/UnifiedError.h"
#include "NativeAssetTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Transactions/Transaction.h"
#include "Math/Operations.h"
#include "Operations/SkyBoxPlacement.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "DObject/StrongObjectPtr.h"
#include "Texture/TextureCube.h"

TEST(FSkyBoxEditorWorkflowTests, TextureAssignmentComposesWithOtherComponentFieldsThroughGenericHistory)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	auto* Transactions = Editor.GetTransactions();
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	TStrongObjectPtr<DTextureCube> Before(NewObject<DTextureCube>(nullptr, "SkyBefore")), After(NewObject<DTextureCube>(nullptr, "SkyAfter"));
	auto* Actor = Level->SpawnActor<ASkyBoxActor>("Sky"); ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetSkyBoxComponent(); Component->SetTextureCube(Before.Get());
	FTransBufferTestAccess::SetHistoryBusy(*Transactions, true);
	auto Blocked = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *Level, After.Get(), "Sky", {false});
	ASSERT_FALSE(Blocked);
	EXPECT_EQ(Blocked.error(), "Editor history is unavailable for recording.");
	auto Same = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *Level, Before.Get(), "Sky", {false});
	ASSERT_FALSE(Same);
	EXPECT_EQ(Same.error(), Blocked.error());
	EXPECT_EQ(Transactions->GetQueueLength(), 0u);
	EXPECT_EQ(Component->GetTextureCube(), Before.Get());
	FTransBufferTestAccess::SetHistoryBusy(*Transactions, false);
	{
		Durin::FScopedTransaction Scope("test", "Texture and intensity", nullptr);
		ASSERT_TRUE(Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *Level, After.Get(), "Sky", {false}));
		Component->SetIntensity(3.0f);
	}
	EXPECT_EQ(Transactions->GetTransaction(0)->GetRecordCount(), 1u);
	EXPECT_FALSE(Transactions->GetTransaction(0)->IsOwnedByModule("LevelEditor"));
	ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Component->GetTextureCube(), Before.Get()); EXPECT_FLOAT_EQ(Component->GetIntensity(), 1.0f);
	ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Component->GetTextureCube(), After.Get()); EXPECT_FLOAT_EQ(Component->GetIntensity(), 3.0f);
}

TEST(FSkyBoxEditorWorkflowTests, RedoRestoresActorWithEmptyTextureAfterAssetRetirement)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Tests::FActorEditorFixture Editor;
	auto* Transactions = Editor.GetTransactions();
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	TStrongObjectPtr<DTextureCube> Cube(NewObject<DTextureCube>(nullptr, "PreparedSkyCube"));
	InitRenderingThread();
	FSkyBoxTestEngine Engine;
	auto* Scene = Engine.CreateTestScene();
	GEngine = &Engine;
	auto* World = Editor.GetWorld();
	World->SetRenderScene(Scene);
	// Keep scene teardown outside the assertions that exercise replay.
	auto CheckReplay = [&] {
		auto Placement = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *Level, Cube.Get(), "Sky", {false});
		ASSERT_TRUE(Placement) << Placement.error();
		ASSERT_EQ(Transactions->GetQueueLength(), 1u);
		EXPECT_FALSE(Transactions->GetTransaction(0)->IsOwnedByModule("DurinEd"));
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(ObserveSkyBoxes(*Scene).bHasSkyBox);
		Cube->MarkAsGarbage();
		auto Rejected = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *Level, Cube.Get(), "Invalid", {false});
		ASSERT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error(), "The dropped TextureCube is unavailable.");
		EXPECT_EQ(Level->FindActorByName("Invalid"), nullptr);
		ASSERT_TRUE(Transactions->Redo());
		ASSERT_EQ(Level->FindActorByName("Sky"), *Placement);
		EXPECT_EQ(Cast<ASkyBoxActor>(*Placement)->GetSkyBoxComponent()->GetTextureCube(), nullptr);
		auto Observation = ObserveSkyBoxes(*Scene);
		EXPECT_TRUE(Observation.bHasSkyBox);
		EXPECT_FALSE(Observation.Data.TextureReference);
	};
	CheckReplay();
	World->SetCurrentLevel(nullptr);
	World->SetRenderScene(nullptr);
	FlushRenderingCommands();
	Engine.ResetTestScene();
	GEngine = nullptr;
	ShutdownRenderingThread();
}

TEST(FSkyBoxEditorWorkflowTests, HostRejectsForeignLevelForCreationAndReplacement)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	TStrongObjectPtr<DLevel> Other(NewObject<DLevel>(nullptr, "ForeignSkyLevel"));
	TStrongObjectPtr<DTextureCube> Before(NewObject<DTextureCube>(nullptr, "ForeignBefore"));
	TStrongObjectPtr<DTextureCube> After(NewObject<DTextureCube>(nullptr, "ForeignAfter"));
	auto* Subsystem = Editor.GetActorEditor();
	auto* Current = Editor.GetWorld()->GetCurrentLevel();
	auto ReadOnly = Durin::LevelEditor::PlaceSkyBox(*Subsystem, *Current, Before.Get(), "ReadOnlySky", {true});
	ASSERT_FALSE(ReadOnly);
	EXPECT_EQ(ReadOnly.error(), "The level is read-only.");
	EXPECT_EQ(Current->FindActorByName("ReadOnlySky"), nullptr);
	auto Create = Durin::LevelEditor::PlaceSkyBox(*Subsystem, *Other, Before.Get(), "Sky", {false});
	ASSERT_FALSE(Create);
	EXPECT_EQ(Create.error(), "The target is not the current editor level.");
	EXPECT_EQ(Other->FindActorByName("Sky"), nullptr);

	auto* Actor = Other->SpawnActor<ASkyBoxActor>("Sky");
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetSkyBoxComponent();
	Component->SetTextureCube(Before.Get());
	auto Replace = Durin::LevelEditor::PlaceSkyBox(*Subsystem, *Other, After.Get(), "Sky", {false});
	ASSERT_FALSE(Replace);
	EXPECT_EQ(Replace.error(), Create.error());
	EXPECT_EQ(Component->GetTextureCube(), Before.Get());
	EXPECT_FALSE(Editor.GetTransactions()->CanUndo());
}

TEST(FSkyBoxEditorWorkflowTests, ImportsCreatesAssignsAndPersistsAcrossReload)
{
	InitializeSkyBoxAssetMount();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;

	Durin::AssetForge::Builtins::FTextureCubeImportValidation Validation = Durin::AssetForge::Builtins::ValidateTextureCubeFaces(
		GetSkyBoxConventionFaces());
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.Dimension, 128u);
	EXPECT_EQ(Validation.MipCount, 8u);

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
		GetSkyBoxConventionFaces(), "/SkyBoxAssetTests/EditorWorkflowCube");
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FPackagePath CubePath;
	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/EditorWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/EditorWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));
	Durin::Tests::FActorEditorFixture Editor;
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(Level));
	auto* Transactions = Editor.GetTransactions();
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, true);

	const auto Blocked = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(),
		*Level, CubeResult.Asset, "Sky", {false});
	ASSERT_FALSE(Blocked);
	EXPECT_EQ(Blocked.error(), "Editor history is unavailable for recording.");
	const auto RejectionMessage = Blocked.error();
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	Durin::FTransBufferTestAccess::SetHistoryBusy(*Transactions, false);

	const auto Placement =
		Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(),
			*Level, CubeResult.Asset, "Sky", {false});
	ASSERT_TRUE(Placement) << Placement.error();
	EXPECT_EQ(Blocked.error(), RejectionMessage);
	auto* Actor = Durin::Cast<Durin::ASkyBoxActor>(*Placement);
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Level->FindActorByName("Sky"), nullptr);
	auto* Conflict = Level->SpawnActor<Durin::ASkyBoxActor>("Sky");
	ASSERT_NE(Conflict, nullptr);
	EXPECT_NE(Conflict->GetFName(), Actor->GetFName());
	EXPECT_EQ(Conflict->GetActorLabel(), "Sky");
	const auto ReservedName = Conflict->GetFName();
	Conflict->Rename("Sky");
	EXPECT_EQ(Conflict->GetFName(), ReservedName);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Level->DestroyActor(Conflict));
	EXPECT_EQ(Level->FindActorByName("Sky"), Actor);
	Actor = Durin::Cast<Durin::ASkyBoxActor>(Level->FindActorByName("Sky"));
	ASSERT_NE(Actor, nullptr);
	EXPECT_EQ(Durin::ASkyBoxActor::StaticClass()->GetDisplayName(), "Sky Box Actor");
	Durin::DSkyBoxComponent* Component = Actor->GetSkyBoxComponent();
	ASSERT_NE(Component, nullptr);
	EXPECT_EQ(Component->GetTextureCube(), CubeResult.Asset);
	Component->SetTint({0.25f, 0.5f, 0.75f, 1.0f});
	Component->SetIntensity(2.5f);
	Durin::FTransform Transform = Actor->GetActorTransform();
	Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(35.0, Durin::FVectorConstants::Up);
	ASSERT_TRUE(Actor->SetActorTransform(Transform));
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(LoadedLevel->FindActorByName("Sky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DSkyBoxComponent* LoadedComponent = LoadedActor->GetSkyBoxComponent();
	ASSERT_NE(LoadedComponent, nullptr);
	ASSERT_NE(LoadedComponent->GetTextureCube(), nullptr);
	EXPECT_EQ(LoadedComponent->GetTint(), (Durin::FLinearColor(0.25f, 0.5f, 0.75f, 1.0f)));
	EXPECT_FLOAT_EQ(LoadedComponent->GetIntensity(), 2.5f);
	EXPECT_EQ(LoadedComponent->GetWorldRotation(), Transform.Rotation);

	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(LoadedLevel));
	const auto BeforeCount = Transactions->GetQueueLength();
	auto Existing = Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(), *LoadedLevel,
		LoadedComponent->GetTextureCube(), "RejectedSky", {false});
	ASSERT_TRUE(Existing) << Existing.error();
	EXPECT_EQ(*Existing, LoadedActor);
	EXPECT_EQ(LoadedLevel->FindActorByName("RejectedSky"), nullptr);
	EXPECT_EQ(Transactions->GetQueueLength(), BeforeCount);
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SkyBoxEditorWorkflowWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(Observation.Count, 1u);
	EXPECT_EQ(
		Observation.Data.TextureReference,
		LoadedComponent->GetTextureCube()->GetTextureReferenceRHI());


	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(
		LevelPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}

TEST(FSkyBoxEditorWorkflowTests, ImportsPanoramaAssignsSkyAndPersistsSettingsAcrossLevelReload)
{
	InitializeSkyBoxAssetMount();
	Durin::InitRenderingThread();
	FSkyBoxTestEngine Engine;
	Durin::FScene* Scene = Engine.CreateTestScene();
	Durin::GEngine = &Engine;

	const std::filesystem::path Panorama = std::filesystem::path(DURIN_TEST_DATA_DIR) /
		"EquirectangularPanorama" / "AnalyticalHDR.hdr";
	const Durin::AssetForge::Builtins::FTextureCubePanoramaImportSettings Settings{
		.FaceDimension = 2,
		.ExposureEV = 1.0f};
	const Durin::AssetForge::Builtins::FTextureCubeImportValidation Validation =
		Durin::AssetForge::Builtins::ValidateTextureCubePanorama(Panorama.generic_string(), Settings);
	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_TRUE(Validation.bHDR);
	EXPECT_EQ(Validation.SourceWidth, 8u);
	EXPECT_EQ(Validation.SourceHeight, 4u);
	EXPECT_EQ(Validation.Dimension, 2u);

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Panorama.generic_string(), "/SkyBoxAssetTests/PanoramaWorkflowCube", Settings);
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	Durin::FPackagePath CubePath;
	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowCube", CubePath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/PanoramaWorkflowLevel", LevelPath));

	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));
	auto* Actor = Level->SpawnActor<Durin::ASkyBoxActor>("PanoramaSky");
	ASSERT_NE(Actor, nullptr);
	Durin::Tests::FActorEditorFixture Editor;
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(Level));
	auto* Transactions = Editor.GetTransactions();
	const auto Placement =
		Durin::LevelEditor::PlaceSkyBox(*Editor.GetActorEditor(),
			*Level, CubeResult.Asset, "UnusedName", {false});
	ASSERT_TRUE(Placement) << Placement.error();
	EXPECT_EQ(*Placement, Actor);
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), nullptr);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Actor->GetSkyBoxComponent()->GetTextureCube(), CubeResult.Asset);
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	Transactions->Reset();
	ASSERT_TRUE(Editor.GetWorld()->SetCurrentLevel(nullptr));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));

	Durin::DLevel* LoadedLevel = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DLevel>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), nullptr, &LoadedValueError);
		LoadedLevel = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedActor = Durin::Cast<Durin::ASkyBoxActor>(
		LoadedLevel->FindActorByName("PanoramaSky"));
	ASSERT_NE(LoadedActor, nullptr);
	Durin::DTextureCube* LoadedCube = LoadedActor->GetSkyBoxComponent()->GetTextureCube();
	ASSERT_NE(LoadedCube, nullptr);
	EXPECT_EQ(LoadedCube->GetSourceLayout(), Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(LoadedCube->GetOriginalSourceWidth(), 8u);
	EXPECT_EQ(LoadedCube->GetOriginalSourceHeight(), 4u);
	EXPECT_EQ(LoadedCube->GetPanoramaFaceDimension(), 2u);
	EXPECT_FLOAT_EQ(LoadedCube->GetPanoramaExposureEV(), 1.0f);
	ASSERT_TRUE(LoadedCube->FinishCachePlatformData());
	EXPECT_EQ(LoadedCube->GetPixelFormat(), Durin::EPixelFormat::BC1_UNORM_SRGB);

	auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "PanoramaWorkflowWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	World->SetRenderScene(Scene);
	ASSERT_TRUE(World->SetCurrentLevel(LoadedLevel));
	const FSkyBoxObservation Observation = ObserveSkyBoxes(*Scene);
	ASSERT_TRUE(Observation.bHasSkyBox);
	EXPECT_EQ(
		Observation.Data.TextureReference,
		LoadedCube->GetTextureReferenceRHI());

	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->SetRenderScene(nullptr);
	Durin::FlushRenderingCommands();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Engine.ResetTestScene();
	Durin::GEngine = nullptr;
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(LevelPath));
	ASSERT_TRUE(Durin::Testing::EnsurePackageNotResident(CubePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LevelPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(CubePath));
	Durin::ShutdownRenderingThread();
}
