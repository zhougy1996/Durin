#include "MaterialTestSupport.h"
#include "NativeTestSupport.h"

TEST(FStaticMeshMaterialTests, ImportedStaticMeshBuildsLODSectionsAndMaterialSlots)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshImports";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/MeshImportTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult ImportResult = Durin::DStaticMesh::ImportAsset(Source.generic_string(), "/MeshImportTests/MultiSection");
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	const Durin::FStaticMeshRenderData* RenderData = ImportResult.Asset->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 2u);
	EXPECT_EQ(RenderData->MaterialSlots[0].Name, "Red");
	EXPECT_EQ(RenderData->MaterialSlots[1].Name, "Blue");
	EXPECT_EQ(RenderData->MaterialSlots[0].SlotId, ImportResult.Asset->GetMaterialSlot(0)->SlotId);
	EXPECT_EQ(RenderData->MaterialSlots[1].SlotId, ImportResult.Asset->GetMaterialSlot(1)->SlotId);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.NumTexCoords, 2u);
	EXPECT_TRUE(LOD.bHasVertexColors);
	EXPECT_EQ(LOD.Positions.size(), 12u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Indices.size(), 12u);
	ASSERT_EQ(LOD.Sections.size(), 4u);
	for (size_t SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
	{
		const Durin::FStaticMeshSection& Section = LOD.Sections[SectionIndex];
		EXPECT_EQ(Section.FirstIndex, static_cast<Durin::uint32>(SectionIndex) * 3u);
		EXPECT_EQ(Section.IndexCount, 3u);
		EXPECT_EQ(Section.MaterialSlotIndex, static_cast<Durin::uint32>(SectionIndex % 2u));
		EXPECT_TRUE(Section.LocalBounds.bIsValid);
	}
	EXPECT_TRUE(LOD.LocalBounds.bIsValid);
	EXPECT_TRUE(RenderData->LocalBounds.bIsValid);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshImportTests/MultiSection", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshSourceProvenanceLivesOutsideContentAndSurvivesAssetOperations)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshSourceProvenance";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint(
		"/StaticMeshSourceProvenance/", (Root / "Content").generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(
		Source.generic_string(), "/StaticMeshSourceProvenance/Environment/Mesh");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	const Durin::FStaticMeshSourceImportData& Provenance = Import.Asset->GetSourceImportData();
	EXPECT_EQ(Provenance.SourcePath.Path,
		"/StaticMeshSourceProvenance/Models/Environment/Mesh.gltf");
	EXPECT_EQ(Provenance.SourceContentHash.size(), 32u);
	EXPECT_EQ(Provenance.ImporterId, "Assimp");
	EXPECT_EQ(Provenance.ImporterVersion, 3u);
	const std::string OriginalSourcePath = Provenance.SourcePath.Path;
	const std::filesystem::path StoredSource =
		Root / "SourceAssets/Models/Environment/Mesh.gltf";
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "Environment" / "Mesh.gltf"));
	EXPECT_EQ(Import.Asset->InspectSource().Status, Durin::EStaticMeshSourceStatus::Available);

	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceProvenance/Environment/Mesh", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceProvenance/Moved/Mesh", NewPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(OldPath, NewPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_EQ(Import.Asset->GetSourceImportData().SourcePath.Path, OriginalSourcePath);
	EXPECT_EQ(Import.Asset->InspectSource().Status, Durin::EStaticMeshSourceStatus::Available);

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(NewPath, Analysis));
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(Durin::Asset::DeleteAsset(NewPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
}

TEST(FStaticMeshMaterialTests, StaticMeshWithoutSourceMetadataLoadsAndMissingSourceCanBeRepaired)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshSourceRepair";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint(
		"/StaticMeshSourceRepair/", (Root / "Content").generic_string() + "/");

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceRepair/Mesh", AssetPath));
	Durin::DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Mesh));
	ASSERT_EQ(Mesh->InspectSource().Status, Durin::EStaticMeshSourceStatus::NoSource);
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Mesh));
	ASSERT_NE(Mesh, nullptr);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	auto* SourceImportProperty = Mesh->GetClass()->FindPropertyByName("SourceImportData");
	ASSERT_NE(SourceImportProperty, nullptr);
	auto* SourceImportData = static_cast<Durin::FStaticMeshSourceImportData*>(
		SourceImportProperty->GetValuePtr(Mesh));
	SourceImportData->SourcePath.Path = "../Outside.gltf";
	EXPECT_EQ(Mesh->InspectSource().Status, Durin::EStaticMeshSourceStatus::Invalid);
	*SourceImportData = {};
	std::string RepairError;
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	ASSERT_TRUE(Mesh->IngestAndChangeSource(
		Source.generic_string(), "/StaticMeshSourceRepair/Models/Mesh.gltf",
		RepairError)) << RepairError;
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->InspectSource().Status, Durin::EStaticMeshSourceStatus::Available);
	EXPECT_EQ(Mesh->GetSourceImportData().SourcePath.Path,
		"/StaticMeshSourceRepair/Models/Mesh.gltf");
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));

	const std::filesystem::path StoredSource = Root / "SourceAssets/Models/Mesh.gltf";
	const std::string OriginalHash = Mesh->GetSourceImportData().SourceContentHash;
	WriteStaticMeshSlotVariant(StoredSource, R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_TRUE(Mesh->PostLoad(RepairError)) << RepairError;
	EXPECT_NE(Mesh->GetSourceImportData().SourceContentHash, OriginalHash);
	EXPECT_TRUE(Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(std::filesystem::remove(StoredSource));
	const Durin::FStaticMeshSourceDiagnostic Missing = Mesh->InspectSource();
	EXPECT_EQ(Missing.Status, Durin::EStaticMeshSourceStatus::Missing);
	EXPECT_NE(Missing.Message.find("source-path repair"), std::string::npos);
	ASSERT_TRUE(Mesh->IngestAndChangeSource(
		Source.generic_string(), "/StaticMeshSourceRepair/Models/Mesh.gltf",
		RepairError)) << RepairError;
	EXPECT_EQ(Mesh->InspectSource().Status, Durin::EStaticMeshSourceStatus::Available);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FStaticMeshMaterialTests, LegacyStaticMeshSourceMetadataIsRejectedAfterMigration)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "LegacyStaticMeshSource";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint(
		"/LegacyStaticMeshSource/", (Root / "Content").generic_string() + "/");

	std::filesystem::create_directories(Root / "Content");
	const std::filesystem::path LegacySource = Root / "Content" / "Legacy.gltf";
	ASSERT_TRUE(std::filesystem::copy_file(
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf", LegacySource));
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyStaticMeshSource/Legacy", AssetPath));
	Durin::DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Mesh));
	auto* SourceProperty = Mesh->GetClass()->FindPropertyByName("SourceFile");
	ASSERT_NE(SourceProperty, nullptr);
	*static_cast<std::string*>(SourceProperty->GetValuePtr(Mesh)) = "Legacy.gltf";
	Mesh->MarkPackageDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Mesh = nullptr;
	const Durin::Asset::FAssetResult LoadResult = Durin::Asset::LoadAsset(AssetPath, Mesh);
	EXPECT_FALSE(LoadResult);
	EXPECT_EQ(Mesh, nullptr);
	EXPECT_NE(LoadResult.Message.find("Legacy static-mesh source metadata is unsupported"), std::string::npos);
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotDefinitionsRoundTripWithDefaults)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotRoundTrip";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotRoundTrip/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath MaterialPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Default", MaterialPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_EQ(Import.Asset->GetNumMaterialSlots(), 2u);
	const std::vector<Durin::FGuid> OriginalIds{
		Import.Asset->GetMaterialSlot(0)->SlotId,
		Import.Asset->GetMaterialSlot(1)->SlotId};
	EXPECT_TRUE(OriginalIds[0].IsValid());
	EXPECT_TRUE(OriginalIds[1].IsValid());
	EXPECT_NE(OriginalIds[0], OriginalIds[1]);
	EXPECT_EQ(Import.Asset->FindMaterialSlot(OriginalIds[1]), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->FindMaterialSlot(Durin::FName("Blue")), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->GetMaterialSlot(2), nullptr);

	Durin::DMaterial* DefaultMaterial = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(MaterialPath, DefaultMaterial));
	ASSERT_TRUE(Durin::Asset::SavePackage(DefaultMaterial->GetPackage()));
	auto* SlotsProperty = static_cast<Durin::FArrayProperty*>(Import.Asset->GetClass()->FindPropertyByName("MaterialSlots"));
	ASSERT_NE(SlotsProperty, nullptr);
	auto* FirstSlot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(SlotsProperty->GetMutableElementPtr(Import.Asset, 0));
	ASSERT_NE(FirstSlot, nullptr);
	FirstSlot->DefaultMaterial = DefaultMaterial;
	Import.Asset->MarkPackageDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Import.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	ASSERT_EQ(Loaded->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->SlotId, OriginalIds[0]);
	EXPECT_EQ(Loaded->GetMaterialSlot(1)->SlotId, OriginalIds[1]);
	ASSERT_NE(Loaded->GetMaterialSlot(0)->DefaultMaterial.Get(), nullptr);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->DefaultMaterial->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotReconciliationPreservesOnlyUnambiguousIdentity)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotReimport";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotReimport/", Root.generic_string() + "/");
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";

	auto ImportBase = [&](std::string_view Name) -> Durin::DStaticMesh* {
		const std::string AssetPath = std::format("/StaticMeshSlotReimport/{}", Name);
		Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(BaseSource.generic_string(), AssetPath);
		EXPECT_TRUE(Import) << Import.Message;
		return Import.Asset;
	};
	auto Rebuild = [&](Durin::DStaticMesh* Mesh, std::string_view Name, std::string_view Materials,
		std::optional<std::pair<std::string_view, std::string_view>> Replacement = std::nullopt, bool LastOnly = false,
		std::optional<Durin::uint32> AppendedMaterialIndex = std::nullopt) {
		const std::filesystem::path SourcePath = Root / "SourceAssets" / "Models" / (std::string(Name) + ".gltf");
		WriteStaticMeshSlotVariant(SourcePath, Materials, Replacement, LastOnly, AppendedMaterialIndex);
		std::string Error;
		ASSERT_TRUE(Mesh->PostLoad(Error)) << Error;
	};

	Durin::DStaticMesh* Reordered = ImportBase("Reordered");
	ASSERT_NE(Reordered, nullptr);
	const Durin::FGuid RedId = Reordered->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid BlueId = Reordered->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Reordered, "Reordered", R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_EQ(Reordered->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->SlotId, BlueId);
	EXPECT_EQ(Reordered->GetMaterialSlot(1)->SlotId, RedId);

	Durin::DStaticMesh* RenameAndReorder = ImportBase("RenameAndReorder");
	ASSERT_NE(RenameAndReorder, nullptr);
	const Durin::FGuid OldRedId = RenameAndReorder->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid OldBlueId = RenameAndReorder->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(RenameAndReorder, "RenameAndReorder", R"({ "name": "Blue" }, { "name": "Crimson" })");
	EXPECT_EQ(RenameAndReorder->GetMaterialSlot(0)->SlotId, OldBlueId);
	EXPECT_NE(RenameAndReorder->GetMaterialSlot(1)->SlotId, OldRedId);
	EXPECT_NE(RenameAndReorder->GetMaterialSlot(1)->SlotId, OldBlueId);

	Durin::DStaticMesh* Added = ImportBase("Added");
	ASSERT_NE(Added, nullptr);
	const Durin::FGuid AddedRedId = Added->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid AddedBlueId = Added->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Added, "Added", R"({ "name": "Red" }, { "name": "Blue" }, { "name": "Green" })",
		std::nullopt, false, 2);
	ASSERT_EQ(Added->GetNumMaterialSlots(), 3u);
	EXPECT_EQ(Added->FindMaterialSlot(Durin::FName("Red"))->SlotId, AddedRedId);
	EXPECT_EQ(Added->FindMaterialSlot(Durin::FName("Blue"))->SlotId, AddedBlueId);
	EXPECT_TRUE(Added->FindMaterialSlot(Durin::FName("Green"))->SlotId.IsValid());

	Durin::DStaticMesh* Removed = ImportBase("Removed");
	ASSERT_NE(Removed, nullptr);
	const Durin::FGuid RemovedBlueId = Removed->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Removed, "Removed", R"({ "name": "Red" }, { "name": "Blue" })",
		std::pair<std::string_view, std::string_view>{R"("material": 0)", R"("material": 1)"});
	ASSERT_EQ(Removed->GetNumMaterialSlots(), 1u);
	EXPECT_EQ(Removed->GetMaterialSlot(0)->SlotId, RemovedBlueId);

	Durin::DStaticMesh* Duplicate = ImportBase("Duplicate");
	ASSERT_NE(Duplicate, nullptr);
	const Durin::FGuid DuplicateRedId = Duplicate->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid DuplicateBlueId = Duplicate->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Duplicate, "Duplicate", R"({ "name": "Shared" }, { "name": "Shared" })");
	ASSERT_EQ(Duplicate->GetNumMaterialSlots(), 2u);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, DuplicateRedId);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, DuplicateBlueId);
	EXPECT_NE(Duplicate->GetMaterialSlot(1)->SlotId, DuplicateRedId);
	EXPECT_NE(Duplicate->GetMaterialSlot(1)->SlotId, DuplicateBlueId);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, Duplicate->GetMaterialSlot(1)->SlotId);
}

TEST(FStaticMeshMaterialTests, FixedRowAssignmentRoundTripsAndSurvivesRenderedReimportReorder)
{
	FRenderSceneHarness Harness;
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotEndToEnd";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotEndToEnd/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath MaterialPath;
	Durin::FAssetPath ComponentPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/RedOverride", MaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/Component", ComponentPath));
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(BaseSource.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	const Durin::FGuid RedId = Import.Asset->FindMaterialSlot(Durin::FName("Red"))->SlotId;

	Durin::DMaterial* Material = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(MaterialPath, Material));
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.85, 0.15, 0.1));
	ASSERT_TRUE(Durin::Asset::SavePackage(Material->GetPackage()));
	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ComponentPath, Component));
	Component->SetStaticMesh(Import.Asset);
	Durin::FStaticMeshMaterialSlotDetailsModel Model(Component);
	const auto RedEntry = std::ranges::find(Model.GetCurrentEntries(), RedId, &Durin::FStaticMeshMaterialSlotDetailsEntry::SlotId);
	ASSERT_NE(RedEntry, Model.GetCurrentEntries().end());
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string EditError;
	const Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&EditError](std::string Error) { EditError = std::move(Error); }};
	ASSERT_TRUE(Model.AssignMaterial(PropertyView, Context, *RedEntry, Material));
	ASSERT_TRUE(EditError.empty());
	ASSERT_TRUE(Durin::Asset::SavePackage(Component->GetPackage()));
	Transactions.Clear();

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	Component = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ComponentPath, Component));
	ASSERT_NE(Component, nullptr);
	ASSERT_NE(Component->GetStaticMesh(), nullptr);
	ASSERT_EQ(Component->GetMaterialBySlotId(RedId)->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot BeforeReimport = CaptureMaterialSlots(Harness.Scene);

	const std::filesystem::path ReimportSource = Root / "SourceAssets" / "Models" / "Mesh.gltf";
	WriteStaticMeshSlotVariant(ReimportSource, R"({ "name": "Blue" }, { "name": "Red" })",
		std::nullopt, false, std::nullopt, true);
	std::string ReimportError;
	ASSERT_TRUE(Component->GetStaticMesh()->PostLoad(ReimportError)) << ReimportError;
	ASSERT_EQ(Component->GetStaticMesh()->GetMaterialSlot(0)->Name, Durin::FName("Blue"));
	ASSERT_EQ(Component->GetStaticMesh()->GetMaterialSlot(1)->SlotId, RedId);
	ASSERT_EQ(Component->GetStaticMesh()->GetRenderData()->LODResources[0].Sections[0].MaterialSlotIndex, 0u);
	ASSERT_EQ(Component->GetStaticMesh()->GetRenderData()->LODResources[0].Sections[1].MaterialSlotIndex, 1u);
	EXPECT_EQ(Component->GetMaterial(1)->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	const FMaterialSlotsSnapshot AfterReimport = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(AfterReimport.Materials.size(), 2u);
	EXPECT_NE(AfterReimport.Proxy, BeforeReimport.Proxy);
	EXPECT_NE(AfterReimport.RenderData, BeforeReimport.RenderData);
	ExpectColorNear(AfterReimport.Materials[1].BaseColor, Durin::FVector4f(0.85f, 0.15f, 0.1f, 1.0f));
	ExpectColorNear(AfterReimport.Materials[0].BaseColor, Durin::FMaterialRenderData{}.BaseColor);
	Component->UnregisterComponent();
	WaitForRenderingThread();

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FStaticMeshMaterialTests, VersionZeroStaticMeshMaterialSlotsMigrateDeterministically)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotMigration";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotMigration/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotMigration/Mesh", MeshPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	auto* VersionProperty = Import.Asset->GetClass()->FindPropertyByName("MaterialSlotsVersion");
	auto* SlotsProperty = static_cast<Durin::FArrayProperty*>(Import.Asset->GetClass()->FindPropertyByName("MaterialSlots"));
	ASSERT_NE(VersionProperty, nullptr);
	ASSERT_NE(SlotsProperty, nullptr);
	*static_cast<Durin::uint32*>(VersionProperty->GetValuePtr(Import.Asset)) = 0;
	SlotsProperty->Resize(Import.Asset, 0);
	Import.Asset->MarkPackageDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Import.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMesh* FirstLoad = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, FirstLoad));
	ASSERT_EQ(FirstLoad->GetNumMaterialSlots(), 2u);
	const std::array FirstIds{FirstLoad->GetMaterialSlot(0)->SlotId, FirstLoad->GetMaterialSlot(1)->SlotId};
	EXPECT_TRUE(FirstIds[0].IsValid());
	EXPECT_TRUE(FirstIds[1].IsValid());
	EXPECT_NE(FirstIds[0], FirstIds[1]);
	EXPECT_TRUE(FirstLoad->GetPackage()->IsDirty()) << "Version-zero slot migration must request an asset resave.";
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMesh* SecondLoad = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, SecondLoad));
	ASSERT_EQ(SecondLoad->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(SecondLoad->GetMaterialSlot(0)->SlotId, FirstIds[0]);
	EXPECT_EQ(SecondLoad->GetMaterialSlot(1)->SlotId, FirstIds[1]);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshImportSettingsValidateDistinctAxes)
{
	Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	EXPECT_TRUE(Settings.IsValid());
	Settings.RightAxis = Durin::EStaticMeshImportAxis::PositiveZ;
	std::string Error;
	EXPECT_FALSE(Settings.IsValid(&Error));
	EXPECT_FALSE(Error.empty());
}

TEST(FStaticMeshMaterialTests, StaticMeshImportSettingsPersistAcrossSourceRebuild)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshAxisImports";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/MeshAxisImportTests/", Root.generic_string() + "/");

	const Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "AsymmetricAxes.obj";
	Durin::FStaticMeshImportResult ImportResult = Durin::DStaticMesh::ImportAsset(
		Source.generic_string(), "/MeshAxisImportTests/AsymmetricAxes", Settings);
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	EXPECT_EQ(ImportResult.Asset->GetImportSettings(), Settings);
	ASSERT_NE(ImportResult.Asset->GetRenderData(), nullptr);
	ASSERT_EQ(ImportResult.Asset->GetRenderData()->LODResources.size(), 1u);
	const std::vector<Durin::FVector3f> ImportedPositions = ImportResult.Asset->GetRenderData()->LODResources[0].Positions;

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshAxisImportTests/AsymmetricAxes", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetImportSettings(), Settings);
	ASSERT_NE(Loaded->GetRenderData(), nullptr);
	ASSERT_EQ(Loaded->GetRenderData()->LODResources.size(), 1u);
	const auto& ReloadedPositions = Loaded->GetRenderData()->LODResources[0].Positions;
	ASSERT_EQ(ReloadedPositions.size(), ImportedPositions.size());
	for (size_t Index = 0; Index < ImportedPositions.size(); ++Index)
	{
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].x, ImportedPositions[Index].x);
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].y, ImportedPositions[Index].y);
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].z, ImportedPositions[Index].z);
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshComponentOverridesRoundTripAfterMeshDependenciesLoad)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotOverrides";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotOverrides/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath FirstMaterialPath;
	Durin::FAssetPath SecondMaterialPath;
	Durin::FAssetPath ComponentPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/First", FirstMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Second", SecondMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Component", ComponentPath));

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult MeshImport = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(MeshImport) << MeshImport.Message;
	Durin::DMaterial* First = nullptr;
	Durin::DMaterial* Second = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstMaterialPath, First));
	ASSERT_TRUE(Durin::Asset::CreateAsset(SecondMaterialPath, Second));
	ASSERT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Second->GetPackage()));

	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ComponentPath, Component));
	Component->SetStaticMesh(MeshImport.Asset);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	ASSERT_TRUE(Durin::Asset::SavePackage(Component->GetPackage()));

	const auto* ComponentData = Durin::Asset::GetAssetRegistry().FindAsset(ComponentPath);
	ASSERT_NE(ComponentData, nullptr);
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, MeshPath), ComponentData->Dependencies.end());
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, FirstMaterialPath), ComponentData->Dependencies.end());
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, SecondMaterialPath), ComponentData->Dependencies.end());

	const std::filesystem::path FixturePath = Durin::Testing::GetTestWorkDirectory()
		/ "StaticMeshSlotOverrides" / "Component.dasset";
	std::vector<Durin::uint8> FixtureBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FixtureBytes, FixturePath.generic_string()));
	EXPECT_FALSE(ContainsSerializedField(FixtureBytes, "Materials"));
	EXPECT_FALSE(ContainsSerializedField(FixtureBytes, "MaterialOverridesVersion"));
	EXPECT_TRUE(ContainsSerializedField(FixtureBytes, "MaterialOverrides"));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ComponentPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	ASSERT_NE(Loaded->GetStaticMesh(), nullptr);
	ASSERT_NE(Loaded->GetStaticMesh()->GetRenderData(), nullptr);
	EXPECT_EQ(Loaded->GetNumMaterials(), 2u);
	ASSERT_NE(Loaded->GetMaterial(0), nullptr);
	ASSERT_NE(Loaded->GetMaterial(1), nullptr);
	EXPECT_EQ(Loaded->GetMaterial(0)->GetPackage()->GetPackagePath(), FirstMaterialPath.ToString());
	EXPECT_EQ(Loaded->GetMaterial(1)->GetPackage()->GetPackagePath(), SecondMaterialPath.ToString());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
}

TEST(FStaticMeshMaterialTests, MaterialInstanceAssetsRoundTripParentAndOverrides)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "Materials";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/MaterialTests/", Root.generic_string() + "/");

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Instance", InstancePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/BaseColorTexture", TexturePath));

	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialBaseColor.png";
	WriteMaterialTextureFixture(TextureSource);
	Durin::FTexture2DImportResult TextureImport = Durin::DTexture2D::ImportAsset(TextureSource.generic_string(), TexturePath.ToString());
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.35f);
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	const Durin::Asset::FAssetData* InstanceData =
		Durin::Asset::GetAssetRegistry().FindAsset(InstancePath);
	ASSERT_NE(InstanceData, nullptr);
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, BasePath), InstanceData->Dependencies.end());
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, TexturePath), InstanceData->Dependencies.end());
	ASSERT_FALSE(Instance->GetPackage()->IsDirty());
	const Durin::uint64 SavedVersion = Instance->GetRenderStateVersion();
	EXPECT_FALSE(Instance->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.2f));
	EXPECT_FALSE(Instance->SetVectorParameterValue(Durin::MaterialParameters::OpacityName(), Durin::FVector3(0.2)));
	EXPECT_FALSE(Instance->GetPackage()->IsDirty());
	EXPECT_EQ(Instance->GetRenderStateVersion(), SavedVersion);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));

	Durin::DMaterialInstance* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(InstancePath, Loaded));
	ASSERT_NE(Loaded->GetParent(), nullptr);
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.35f));
	Durin::DTexture2D* LoadedTexture = nullptr;
	ASSERT_TRUE(Loaded->GetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), LoadedTexture));
	ASSERT_NE(LoadedTexture, nullptr);
	EXPECT_EQ(Loaded->GetRenderData().BaseColorTexture, LoadedTexture->GetRenderResource());
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.6, 0.4, 0.2));
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.6f, 0.4f, 0.2f, 0.35f));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
}

TEST(FStaticMeshMaterialTests, LegacyParameterMapsAreSkippedWithoutMigration)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LegacyMaterials";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPoint("/LegacyMaterialTests/", Root.generic_string() + "/");

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyMaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyMaterialTests/Instance", InstancePath));

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));

	std::vector<Durin::uint8> BaseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BaseBytes, (Root / "Base.dasset").generic_string()));
	ASSERT_TRUE(RewriteSerializedFieldAsLegacyMap(BaseBytes, "ParameterDefinitions", "VectorParameters"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(BaseBytes)), Root / "Base.dasset"));

	std::vector<Durin::uint8> InstanceBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(InstanceBytes, (Root / "Instance.dasset").generic_string()));
	ASSERT_TRUE(RewriteSerializedFieldAsLegacyMap(InstanceBytes, "ParameterOverrides", "ScalarParameters"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(InstanceBytes)), Root / "Instance.dasset"));

	Durin::DMaterialInstance* LoadedInstance = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(InstancePath, LoadedInstance));
	ASSERT_NE(LoadedInstance, nullptr);
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(LoadedInstance->GetParent());
	ASSERT_NE(LoadedBase, nullptr);

	Durin::FVector3 BaseColor;
	ASSERT_TRUE(LoadedBase->GetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, Durin::FVector3(0.95, 0.62, 0.22));
	EXPECT_TRUE(LoadedInstance->GetParameterOverrides().empty());
	EXPECT_FALSE(LoadedInstance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));

	float Opacity = 0.0f;
	ASSERT_TRUE(LoadedInstance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
}
