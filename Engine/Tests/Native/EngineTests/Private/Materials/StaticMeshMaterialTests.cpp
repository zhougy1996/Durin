#include "MaterialTestSupport.h"
#include "StaticMeshSourceTranslation.h"
#include "NativeTestSupport.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

namespace
{
	auto RelocateAssetForTest(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination) -> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::Asset::FAssetRelocationBatchToken Token;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::AnalyzeAssetRelocationBatch(std::span{&Mapping, 1}, Token);
		if (Result) Result = Durin::Asset::ApplyAssetRelocationBatch(Token);
		return Result;
	}
}

TEST(FStaticMeshMaterialTests, ImportedStaticMeshBuildsLODSectionsAndMaterialSlots)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshImports";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/MeshImportTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult ImportResult = Durin::Asset::Import::ImportStaticMeshAsset(Source.generic_string(), "/MeshImportTests/MultiSection");
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	const Durin::FStaticMeshRenderData* RenderData = ImportResult.Asset->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 2u);
	EXPECT_EQ(RenderData->MaterialSlots[0].Name, "Red");
	EXPECT_EQ(RenderData->MaterialSlots[1].Name, "Blue");
	EXPECT_EQ(RenderData->MaterialSlots[0].SourceMaterialIndex, 0u);
	EXPECT_EQ(RenderData->MaterialSlots[1].SourceMaterialIndex, 1u);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const auto& Positions =
		LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto& TangentsVertexBuffer =
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer;
	EXPECT_EQ(LOD.NumTexCoords, 2u);
	EXPECT_TRUE(LOD.bHasColorVertexData);
	EXPECT_EQ(Positions.size(), 12u);
	EXPECT_EQ(
		TangentsVertexBuffer.GetNormals().size(),
		Positions.size());
	EXPECT_EQ(
		TangentsVertexBuffer.GetTangents().size(),
		Positions.size());
	EXPECT_EQ(
		LOD.VertexBuffers.ColorVertexBuffer.GetColors().size(),
		Positions.size());
	EXPECT_EQ(LOD.IndexBuffer.GetIndices().size(), 12u);
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
	Durin::PathUtilities::RegisterMountPointForTests(
		"/StaticMeshSourceProvenance/", (Root / "Content").generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::Asset::Import::ImportStaticMeshAsset(
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
		Root / "Content/Models/Environment/Mesh.gltf";
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "Environment" / "Mesh.gltf"));
	EXPECT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Import.Asset).Status,
		Durin::EStaticMeshSourceStatus::Available);

	Durin::FAssetPath OldPath;
	Durin::FAssetPath NewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceProvenance/Environment/Mesh", OldPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceProvenance/Moved/Mesh", NewPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_EQ(Import.Asset->GetSourceImportData().SourcePath.Path, OriginalSourcePath);
	EXPECT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Import.Asset).Status,
		Durin::EStaticMeshSourceStatus::Available);

	Durin::Asset::FAssetDeleteAnalysis Analysis;
	ASSERT_TRUE(Durin::Asset::AnalyzeAssetDeletion(NewPath, Analysis));
	EXPECT_TRUE(Analysis.CompanionFiles.empty());
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
}

TEST(FStaticMeshMaterialTests, StaticMeshWithoutSourceMetadataLoadsAndMissingSourceCanBeRepaired)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshSourceRepair";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/StaticMeshSourceRepair/", (Root / "Content").generic_string() + "/");

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSourceRepair/Mesh", AssetPath));
	Durin::DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Mesh));
	ASSERT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Mesh).Status,
		Durin::EStaticMeshSourceStatus::NoSource);
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
	EXPECT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Mesh).Status,
		Durin::EStaticMeshSourceStatus::Invalid);
	*SourceImportData = {};
	std::string RepairError;
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	ASSERT_TRUE(Durin::Asset::Import::IngestAndChangeStaticMeshSource(*Mesh,
		Source.generic_string(), "/StaticMeshSourceRepair/Models/Mesh.gltf",
		RepairError)) << RepairError;
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Mesh).Status,
		Durin::EStaticMeshSourceStatus::Available);
	EXPECT_EQ(Mesh->GetSourceImportData().SourcePath.Path,
		"/StaticMeshSourceRepair/Models/Mesh.gltf");
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));

	const std::filesystem::path StoredSource = Root / "Content/Models/Mesh.gltf";
	const std::string OriginalHash = Mesh->GetSourceImportData().SourceContentHash;
	WriteStaticMeshSlotVariant(StoredSource, R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_TRUE(Mesh->PostLoad(RepairError)) << RepairError;
	EXPECT_NE(Mesh->GetSourceImportData().SourceContentHash, OriginalHash);
	EXPECT_TRUE(Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(std::filesystem::remove(StoredSource));
	const Durin::FStaticMeshSourceDiagnostic Missing =
		Durin::Asset::Import::InspectStaticMeshSource(*Mesh);
	EXPECT_EQ(Missing.Status, Durin::EStaticMeshSourceStatus::Missing);
	EXPECT_NE(Missing.Message.find("source-path repair"), std::string::npos);
	ASSERT_TRUE(Durin::Asset::Import::IngestAndChangeStaticMeshSource(*Mesh,
		Source.generic_string(), "/StaticMeshSourceRepair/Models/Mesh.gltf",
		RepairError)) << RepairError;
	EXPECT_EQ(Durin::Asset::Import::InspectStaticMeshSource(*Mesh).Status,
		Durin::EStaticMeshSourceStatus::Available);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotDefinitionsRoundTripWithDefaults)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotRoundTrip";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/StaticMeshSlotRoundTrip/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath MaterialPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Default", MaterialPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::Asset::Import::ImportStaticMeshAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_EQ(Import.Asset->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Import.Asset->FindMaterialSlot(Durin::FName("Blue")), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->GetMaterialIndex(Durin::FName("Blue")), 1u);
	EXPECT_EQ(Import.Asset->GetMaterialSlot(2), nullptr);
	std::string RenameError;
	EXPECT_FALSE(Import.Asset->RenameMaterialSlot(0, Durin::FName(), RenameError));
	EXPECT_FALSE(Import.Asset->RenameMaterialSlot(0, Durin::FName("Blue"), RenameError));
	ASSERT_TRUE(Import.Asset->RenameMaterialSlot(0, Durin::FName("Body"), RenameError)) << RenameError;
	EXPECT_EQ(Import.Asset->GetMaterialIndex(Durin::FName("Body")), 0u);

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
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->Name, Durin::FName("Body"));
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->SourceName, "Red");
	ASSERT_NE(Loaded->GetMaterialSlot(0)->DefaultMaterial.Get(), nullptr);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->DefaultMaterial->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotReconciliationPreservesStableIndices)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotReimport";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/StaticMeshSlotReimport/", Root.generic_string() + "/");
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";

	auto ImportBase = [&](std::string_view Name) -> Durin::DStaticMesh* {
		const std::string AssetPath = std::format("/StaticMeshSlotReimport/{}", Name);
		Durin::FStaticMeshImportResult Import = Durin::Asset::Import::ImportStaticMeshAsset(BaseSource.generic_string(), AssetPath);
		EXPECT_TRUE(Import) << Import.Message;
		return Import.Asset;
	};
	auto Rebuild = [&](Durin::DStaticMesh* Mesh, std::string_view Name, std::string_view Materials,
		std::optional<std::pair<std::string_view, std::string_view>> Replacement = std::nullopt, bool LastOnly = false,
		std::optional<Durin::uint32> AppendedMaterialIndex = std::nullopt) {
		const std::filesystem::path SourcePath = Root / "Models" / (std::string(Name) + ".gltf");
		WriteStaticMeshSlotVariant(SourcePath, Materials, Replacement, LastOnly, AppendedMaterialIndex);
		std::string Error;
		ASSERT_TRUE(Mesh->PostLoad(Error)) << Error;
	};

	Durin::DStaticMesh* Reordered = ImportBase("Reordered");
	ASSERT_NE(Reordered, nullptr);
	Rebuild(Reordered, "Reordered", R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_EQ(Reordered->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->Name, Durin::FName("Red"));
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->SourceMaterialIndex, 1u);
	EXPECT_EQ(Reordered->GetMaterialSlot(1)->Name, Durin::FName("Blue"));
	EXPECT_EQ(Reordered->GetMaterialSlot(1)->SourceMaterialIndex, 0u);
	ASSERT_EQ(Reordered->GetRenderData()->LODResources[0].Sections.size(), 4u);
	EXPECT_EQ(Reordered->GetRenderData()->LODResources[0].Sections[0].MaterialSlotIndex, 1u);
	EXPECT_EQ(Reordered->GetRenderData()->LODResources[0].Sections[1].MaterialSlotIndex, 0u);

	Durin::DStaticMesh* RenameAndReorder = ImportBase("RenameAndReorder");
	ASSERT_NE(RenameAndReorder, nullptr);
	Rebuild(RenameAndReorder, "RenameAndReorder", R"({ "name": "Blue" }, { "name": "Crimson" })");
	ASSERT_EQ(RenameAndReorder->GetNumMaterialSlots(), 3u);
	EXPECT_EQ(RenameAndReorder->GetMaterialSlot(0)->Name, Durin::FName("Red"));
	EXPECT_EQ(RenameAndReorder->GetMaterialSlot(1)->Name, Durin::FName("Blue"));
	EXPECT_EQ(RenameAndReorder->GetMaterialSlot(2)->Name, Durin::FName("Crimson"));
	EXPECT_EQ(RenameAndReorder->GetRenderData()->LODResources[0].Sections[0].MaterialSlotIndex, 1u);
	EXPECT_EQ(RenameAndReorder->GetRenderData()->LODResources[0].Sections[1].MaterialSlotIndex, 2u);

	Durin::DStaticMesh* Renamed = ImportBase("Renamed");
	ASSERT_NE(Renamed, nullptr);
	std::string RenameError;
	auto* PreservedDefault = Durin::NewObject<Durin::DMaterial>(nullptr, "PreservedSlotDefault");
	ASSERT_TRUE(Renamed->SetImportedDefaultMaterial(0, PreservedDefault, RenameError)) << RenameError;
	ASSERT_TRUE(Renamed->RenameMaterialSlot(0, Durin::FName("Body"), RenameError)) << RenameError;
	Rebuild(Renamed, "Renamed", R"({ "name": "Crimson" }, { "name": "Blue" })");
	ASSERT_EQ(Renamed->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Renamed->GetMaterialSlot(0)->Name, Durin::FName("Body"));
	EXPECT_EQ(Renamed->GetMaterialSlot(0)->SourceName, "Crimson");
	EXPECT_EQ(Renamed->GetMaterialSlot(0)->DefaultMaterial.Get(), PreservedDefault);

	Durin::DStaticMesh* Added = ImportBase("Added");
	ASSERT_NE(Added, nullptr);
	Rebuild(Added, "Added", R"({ "name": "Red" }, { "name": "Blue" }, { "name": "Green" })",
		std::nullopt, false, 2);
	ASSERT_EQ(Added->GetNumMaterialSlots(), 3u);
	EXPECT_EQ(Added->GetMaterialSlot(0)->Name, Durin::FName("Red"));
	EXPECT_EQ(Added->GetMaterialSlot(1)->Name, Durin::FName("Blue"));
	EXPECT_EQ(Added->GetMaterialSlot(2)->Name, Durin::FName("Green"));

	Durin::DStaticMesh* Removed = ImportBase("Removed");
	ASSERT_NE(Removed, nullptr);
	Rebuild(Removed, "Removed", R"({ "name": "Red" }, { "name": "Blue" })",
		std::pair<std::string_view, std::string_view>{R"("material": 0)", R"("material": 1)"});
	ASSERT_EQ(Removed->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Removed->GetMaterialSlot(0)->Name, Durin::FName("Red"));
	EXPECT_EQ(Removed->GetMaterialSlot(1)->Name, Durin::FName("Blue"));
	ASSERT_FALSE(Removed->GetRenderData()->LODResources[0].Sections.empty());
	EXPECT_TRUE(std::ranges::all_of(
		Removed->GetRenderData()->LODResources[0].Sections,
		[](const Durin::FStaticMeshSection& Section) { return Section.MaterialSlotIndex == 1u; }));
	Rebuild(Removed, "Removed", R"({ "name": "Red" }, { "name": "Blue" })");
	ASSERT_EQ(Removed->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Removed->GetRenderData()->LODResources[0].Sections[0].MaterialSlotIndex, 0u);
	EXPECT_EQ(Removed->GetRenderData()->LODResources[0].Sections[1].MaterialSlotIndex, 1u);

	Durin::DStaticMesh* Duplicate = ImportBase("Duplicate");
	ASSERT_NE(Duplicate, nullptr);
	Rebuild(Duplicate, "Duplicate", R"({ "name": "Shared" }, { "name": "Shared" })");
	ASSERT_EQ(Duplicate->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Duplicate->GetMaterialSlot(0)->Name, Durin::FName("Red"));
	EXPECT_EQ(Duplicate->GetMaterialSlot(1)->Name, Durin::FName("Blue"));
	EXPECT_EQ(Duplicate->GetMaterialSlot(0)->SourceName, "Shared");
	EXPECT_EQ(Duplicate->GetMaterialSlot(1)->SourceName, "Shared");
}

TEST(FStaticMeshMaterialTests, FixedRowAssignmentRoundTripsByIndex)
{
	FRenderSceneHarness Harness;
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotEndToEnd";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/StaticMeshSlotEndToEnd/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath MaterialPath;
	Durin::FAssetPath ComponentPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/RedOverride", MaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotEndToEnd/Component", ComponentPath));
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::Asset::Import::ImportStaticMeshAsset(BaseSource.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	const Durin::FStaticMeshMaterialSlotDefinition* RedSlot =
		Import.Asset->FindMaterialSlot(Durin::FName("Red"));
	ASSERT_NE(RedSlot, nullptr);
	const Durin::uint32 RedIndex = static_cast<Durin::uint32>(
		RedSlot - Import.Asset->GetMaterialSlots().data());

	Durin::DMaterial* Material = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(MaterialPath, Material));
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.85, 0.15, 0.1));
	ASSERT_TRUE(Durin::Asset::SavePackage(Material->GetPackage()));
	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ComponentPath, Component));
	Component->SetStaticMesh(Import.Asset);
	Durin::Editor::Level::FStaticMeshMaterialSlotDetailsModel Model(Component);
	const auto RedEntry = std::ranges::find(
		Model.GetCurrentEntries(), RedIndex, &Durin::Editor::Level::FStaticMeshMaterialSlotDetailsEntry::SlotIndex);
	ASSERT_NE(RedEntry, Model.GetCurrentEntries().end());
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string EditError;
	const Durin::Editor::FPropertyViewContext Context{
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
	ASSERT_EQ(Component->GetMaterial(RedIndex)->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	WriteStaticMeshSlotVariant(
		Root / "Models/Mesh.gltf", R"({ "name": "Blue" }, { "name": "Red" })");
	std::string ReimportError;
	ASSERT_TRUE(Component->GetStaticMesh()->PostLoad(ReimportError)) << ReimportError;
	ASSERT_EQ(Component->GetStaticMesh()->GetMaterialIndex(Durin::FName("Red")), RedIndex);
	EXPECT_EQ(Component->GetStaticMesh()->GetMaterialSlot(RedIndex)->SourceMaterialIndex, 1u);
	const auto& ReimportedSections =
		Component->GetStaticMesh()->GetRenderData()->LODResources[0].Sections;
	ASSERT_EQ(ReimportedSections.size(), 4u);
	EXPECT_EQ(ReimportedSections[0].MaterialSlotIndex, 1u);
	EXPECT_EQ(ReimportedSections[1].MaterialSlotIndex, RedIndex);
	EXPECT_EQ(Component->GetMaterial(RedIndex)->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	auto* RenderComponent = Harness.CreateStaticMeshComponent("ReimportRenderComponent");
	RenderComponent->SetStaticMesh(Component->GetStaticMesh());
	ASSERT_TRUE(RenderComponent->SetMaterial(RedIndex, Component->GetMaterial(RedIndex)));
	const FMaterialSlotsSnapshot Rendered = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Rendered.Materials.size(), 2u);
	ExpectColorNear(
		GetMaterialBinding(Rendered.Materials[RedIndex]).BaseColor,
		Durin::FVector4f(0.85f, 0.15f, 0.1f, 1.0f));
	RenderComponent->UnregisterComponent();
	WaitForRenderingThread();

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	Harness.Shutdown();
	Durin::CollectGarbage();
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
	Durin::PathUtilities::RegisterMountPointForTests("/MeshAxisImportTests/", Root.generic_string() + "/");

	const Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "AsymmetricAxes.obj";
	Durin::FStaticMeshImportResult ImportResult = Durin::Asset::Import::ImportStaticMeshAsset(
		Source.generic_string(), "/MeshAxisImportTests/AsymmetricAxes", Settings);
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	EXPECT_EQ(ImportResult.Asset->GetImportSettings(), Settings);
	ASSERT_NE(ImportResult.Asset->GetRenderData(), nullptr);
	ASSERT_EQ(ImportResult.Asset->GetRenderData()->LODResources.size(), 1u);
	const std::vector<Durin::FVector3f> ImportedPositions =
		ImportResult.Asset->GetRenderData()->LODResources[0]
			.VertexBuffers.PositionVertexBuffer.GetPositions();

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshAxisImportTests/AsymmetricAxes", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetImportSettings(), Settings);
	ASSERT_NE(Loaded->GetRenderData(), nullptr);
	ASSERT_EQ(Loaded->GetRenderData()->LODResources.size(), 1u);
	const auto& ReloadedPositions =
		Loaded->GetRenderData()->LODResources[0]
			.VertexBuffers.PositionVertexBuffer.GetPositions();
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
	Durin::PathUtilities::RegisterMountPointForTests("/StaticMeshSlotOverrides/", Root.generic_string() + "/");

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath FirstMaterialPath;
	Durin::FAssetPath SecondMaterialPath;
	Durin::FAssetPath ComponentPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/First", FirstMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Second", SecondMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotOverrides/Component", ComponentPath));

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult MeshImport = Durin::Asset::Import::ImportStaticMeshAsset(Source.generic_string(), MeshPath.ToString());
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

	const auto* ComponentData = Durin::Asset::GetAssetRegistry().FindAssetExact(ComponentPath);
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
	EXPECT_FALSE(ContainsSerializedField(FixtureBytes, "MaterialOverrides"));
	EXPECT_TRUE(ContainsSerializedField(FixtureBytes, "OverrideMaterials"));

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
	Durin::PathUtilities::RegisterMountPointForTests("/MaterialTests/", Root.generic_string() + "/");

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Instance", InstancePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/BaseColorTexture", TexturePath));

	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialBaseColor.png";
	WriteMaterialTextureFixture(TextureSource);
	Durin::FTexture2DImportResult TextureImport = Durin::Asset::Import::ImportTexture2DAsset(TextureSource.generic_string(), TexturePath.ToString());
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Base->SetVectorParameterValue(Durin::MaterialParameters::NormalName(), Durin::FVector3(0.0, 1.0, 1.0));
	Base->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 0.7f);
	Durin::FMaterialStaticProperties StaticProperties;
	StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	StaticProperties.bTwoSided = true;
	StaticProperties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Enabled;
	StaticProperties.OpacityMaskThreshold = 0.4f;
	ASSERT_TRUE(Base->SetStaticProperties(StaticProperties));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.35f);
	Instance->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.8f);
	Instance->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 2.0f);
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, -1.0));
	Instance->SetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.25, 0.5));
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	const Durin::Asset::FAssetData* InstanceData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(InstancePath);
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
	EXPECT_EQ(Loaded->GetStaticProperties(), StaticProperties);
	ExpectColorNear(GetMaterialBinding(Loaded->GetRenderData()).BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.35f));
	const Durin::FMaterialRenderV2Binding LoadedBinding = GetMaterialBinding(Loaded->GetRenderData());
	EXPECT_FLOAT_EQ(LoadedBinding.Metallic, 0.8f);
	EXPECT_FLOAT_EQ(LoadedBinding.Roughness, 0.7f);
	EXPECT_FLOAT_EQ(LoadedBinding.UVChannels[0], 2.0f);
	EXPECT_EQ(LoadedBinding.UVScales[0], Durin::FVector2f(2.0f, -1.0f));
	EXPECT_EQ(LoadedBinding.UVOffsets[0], Durin::FVector2f(0.25f, 0.5f));
	Durin::DTexture2D* LoadedTexture = nullptr;
	ASSERT_TRUE(Loaded->GetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), LoadedTexture));
	ASSERT_NE(LoadedTexture, nullptr);
	EXPECT_EQ(
		GetMaterialBinding(Loaded->GetRenderData()).Textures[0],
		LoadedTexture->GetTextureReferenceRHI());
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.6, 0.4, 0.2));
	ExpectColorNear(GetMaterialBinding(Loaded->GetRenderData()).BaseColor, Durin::FVector4f(0.6f, 0.4f, 0.2f, 0.35f));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
}

TEST(FStaticMeshMaterialTests, LegacyParameterMapsAreSkippedWithoutMigration)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LegacyMaterials";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/LegacyMaterialTests/", Root.generic_string() + "/");

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
