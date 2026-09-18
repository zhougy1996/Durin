#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "StaticMesh/StaticMeshMaterialBinding.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshTestEnvironment.h"
#include "StaticMeshMaterialTestFixture.h"
#include "Components/SplineMeshComponent.h"

TEST(FStaticMeshMaterialTests, ImportedStaticMeshBuildsLODSectionsAndMaterialSlots)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshImports";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/MeshImportTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> ImportResult = Durin::AssetForge::Builtins::ImportStaticMeshForTest(Source.generic_string(), "/MeshImportTests/MultiSection");
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
		EXPECT_EQ(Section.FirstIndex, static_cast<uint32>(SectionIndex) * 3u);
		EXPECT_EQ(Section.IndexCount, 3u);
		EXPECT_EQ(Section.MaterialSlotIndex, static_cast<uint32>(SectionIndex % 2u));
		EXPECT_TRUE(Section.LocalBounds.bIsValid);
	}
	EXPECT_TRUE(LOD.LocalBounds.bIsValid);
	EXPECT_TRUE(RenderData->LocalBounds.bIsValid);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MeshImportTests/MultiSection", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotDefinitionsRoundTripWithDefaults)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotRoundTrip";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/StaticMeshSlotRoundTrip/", Root.generic_string() + "/");

	Durin::FPackagePath MeshPath;
	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSlotRoundTrip/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSlotRoundTrip/Default", MaterialPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_EQ(Import.Asset->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Import.Asset->FindMaterialSlot(Durin::FName("Blue")), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->GetMaterialIndex(Durin::FName("Blue")), 1u);
	EXPECT_EQ(Import.Asset->GetMaterialSlot(2), nullptr);
	Import.Asset->GetPackage()->ClearDirty();
	const auto OriginalName = Import.Asset->GetMaterialSlot(0)->Name;
	const auto Empty = Import.Asset->RenameMaterialSlot(0, Durin::FName());
	EXPECT_EQ(Empty.Error.Code, Durin::EStaticMeshSlotRenameError::EmptyName);
	EXPECT_EQ(Empty.Error.Owner, Durin::FObjectKey(Import.Asset));
	EXPECT_EQ(Empty.Error.Index, 0u);
	EXPECT_EQ(Empty.Error.SlotCount, 2u);
	const auto Duplicate = Import.Asset->RenameMaterialSlot(0, Durin::FName("Blue"));
	EXPECT_EQ(Duplicate.Error.Code, Durin::EStaticMeshSlotRenameError::DuplicateName);
	EXPECT_EQ(Duplicate.Error.Name, "Blue");
	EXPECT_EQ(Duplicate.Error.ConflictingIndex, 1u);
	const auto OutOfRange = Import.Asset->RenameMaterialSlot(2, Durin::FName("Rejected"));
	EXPECT_EQ(OutOfRange.Error.Code, Durin::EStaticMeshSlotRenameError::Index);
	EXPECT_EQ(OutOfRange.Error.Index, 2u);
	EXPECT_EQ(OutOfRange.Error.SlotCount, 2u);
	EXPECT_EQ(OutOfRange.Error.Name, "Rejected");
	EXPECT_EQ(Import.Asset->GetMaterialSlot(0)->Name, OriginalName);
	EXPECT_FALSE(Import.Asset->GetPackage()->IsDirty());
	ASSERT_TRUE(Import.Asset->RenameMaterialSlot(0, Durin::FName("Body")));
	EXPECT_EQ(Import.Asset->GetMaterialIndex(Durin::FName("Body")), 0u);
	EXPECT_EQ(Duplicate.Error.Name, "Blue");
	EXPECT_EQ(Duplicate.Error.ConflictingIndex, 1u);
	EXPECT_EQ(OutOfRange.Error.Name, "Rejected");
	EXPECT_TRUE(Import.Asset->GetPackage()->IsDirty());
	Import.Asset->GetPackage()->ClearDirty();
	EXPECT_TRUE(Import.Asset->RenameMaterialSlot(0, Durin::FName("Body")));
	EXPECT_FALSE(Import.Asset->GetPackage()->IsDirty());

	Durin::DMaterial* DefaultMaterial = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MaterialPath, DefaultMaterial));
	ASSERT_TRUE(Durin::SavePackage(DefaultMaterial->GetPackage()));
	auto* SlotsProperty = static_cast<Durin::FArrayProperty*>(Import.Asset->GetClass()->FindPropertyByName("MaterialSlots"));
	ASSERT_NE(SlotsProperty, nullptr);
	auto* FirstSlot = static_cast<Durin::FMeshMaterialSlotDefinition*>(SlotsProperty->GetMutableElementPtr(Import.Asset, 0));
	ASSERT_NE(FirstSlot, nullptr);
	FirstSlot->DefaultMaterial = DefaultMaterial;
	Import.Asset->MarkPackageDirty();
	ASSERT_TRUE(Durin::SavePackage(Import.Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(MaterialPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Loaded));
	ASSERT_NE(Loaded, nullptr);
	ASSERT_EQ(Loaded->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->Name, Durin::FName("Body"));
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->SourceName, "Red");
	ASSERT_NE(Loaded->GetMaterialSlot(0)->DefaultMaterial.Get(), nullptr);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->DefaultMaterial->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(MaterialPath));
}

TEST(FStaticMeshMaterialTests, StaticMeshMaterialSlotReconciliationPreservesStableIndices)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotReimport";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/StaticMeshSlotReimport/", Root.generic_string() + "/");
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";

	auto ImportBase = [&](std::string_view Name) -> Durin::DStaticMesh* {
		const std::string AssetPath = std::format("/StaticMeshSlotReimport/{}", Name);
		const std::filesystem::path SourcePath =
			Root / "Models" / (std::string(Name) + ".gltf");
		std::filesystem::create_directories(SourcePath.parent_path());
		std::filesystem::copy_file(BaseSource, SourcePath,
			std::filesystem::copy_options::overwrite_existing);
		Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(SourcePath.generic_string(), AssetPath);
		EXPECT_TRUE(Import) << Import.Message;
		return Import.Asset;
	};
	auto Rebuild = [&](Durin::DStaticMesh* Mesh, std::string_view Name, std::string_view Materials,
		std::optional<std::pair<std::string_view, std::string_view>> Replacement = std::nullopt, bool LastOnly = false,
		std::optional<uint32> AppendedMaterialIndex = std::nullopt) {
		const std::filesystem::path SourcePath = Root / "Models" / (std::string(Name) + ".gltf");
		WriteStaticMeshSlotVariant(SourcePath, Materials, Replacement, LastOnly, AppendedMaterialIndex);
		ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
			*Mesh));
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
	Durin::FPackagePath PreservedDefaultPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/StaticMeshSlotReimport/PreservedSlotDefault", PreservedDefaultPath));
	Durin::DMaterial* PreservedDefault = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(PreservedDefaultPath, PreservedDefault));
	ASSERT_TRUE(Durin::SavePackage(PreservedDefault->GetPackage()));
	const auto OtherDefault = Reordered->GetMaterialSlot(1)->DefaultMaterial;
	Reordered->SetMaterialSlotDefaultMaterial(0, PreservedDefault);
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->SourceMaterialIndex, 1u);
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->DefaultMaterial.Get(), PreservedDefault);
	EXPECT_EQ(Reordered->GetMaterialSlot(1)->DefaultMaterial, OtherDefault);
	Renamed->SetMaterialSlotDefaultMaterial(0, PreservedDefault);
	ASSERT_TRUE(Renamed->RenameMaterialSlot(0, Durin::FName("Body")));
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
	Durin::Testing::RegisterMountPointForTests("/StaticMeshSlotEndToEnd/", Root.generic_string() + "/");

	Durin::FPackagePath MeshPath;
	Durin::FPackagePath MaterialPath;
	Durin::FPackagePath ComponentPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSlotEndToEnd/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSlotEndToEnd/RedOverride", MaterialPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSlotEndToEnd/Component", ComponentPath));
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	const std::filesystem::path MutableSource = Root / "Models/Mesh.gltf";
	std::filesystem::create_directories(MutableSource.parent_path());
	std::filesystem::copy_file(BaseSource, MutableSource,
		std::filesystem::copy_options::overwrite_existing);
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(MutableSource.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	const Durin::FMeshMaterialSlotDefinition* RedSlot =
		Import.Asset->FindMaterialSlot(Durin::FName("Red"));
	ASSERT_NE(RedSlot, nullptr);
	const uint32 RedIndex = static_cast<uint32>(
		RedSlot - Import.Asset->GetMaterialSlots().data());

	Durin::DMaterial* Material = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(SetBindingProgram(*Material));
	Material->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.85, 0.15, 0.1));
	ASSERT_TRUE(Durin::SavePackage(Material->GetPackage()));
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ComponentPath, Component));
	Component->SetStaticMesh(Import.Asset);
	Durin::Editor::Level::FStaticMeshMaterialSlotDetailsModel Model(Component);
	const auto RedEntry = std::ranges::find(
		Model.GetCurrentEntries(), RedIndex, &Durin::Editor::Level::FStaticMeshMaterialSlotDetailsEntry::SlotIndex);
	ASSERT_NE(RedEntry, Model.GetCurrentEntries().end());
	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string EditError;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&EditError](std::string Error) { EditError = std::move(Error); }};
	ASSERT_TRUE(Model.AssignMaterial(PropertyView, Context, *RedEntry, Material));
	ASSERT_TRUE(EditError.empty());
	ASSERT_TRUE(Durin::SavePackage(Component->GetPackage()));
	EXPECT_TRUE(Transactions->Reset());

	ASSERT_TRUE(Durin::UnloadPackage(ComponentPath));
	Material = nullptr;
	const Durin::FAssetResult MaterialUnload =
		Durin::UnloadPackage(MaterialPath);
	ASSERT_TRUE(MaterialUnload) << MaterialUnload.Message;
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	Component = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ComponentPath), Component));
	ASSERT_NE(Component, nullptr);
	ASSERT_NE(Component->GetStaticMesh(), nullptr);
	(void)Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_EQ(Component->GetMaterial(RedIndex)->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	WriteStaticMeshSlotVariant(
		MutableSource, R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Component->GetStaticMesh()));
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
	ASSERT_TRUE(RenderComponent->ResetMaterial(RedIndex));
	RenderComponent->SetStaticMesh(nullptr);
	WaitForRenderingThread();

	ASSERT_TRUE(Durin::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		MeshPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	Harness.Shutdown();
	Durin::CollectGarbage();
}

namespace
{
	template<typename TComponent>
	auto VerifyComponentOverridesRoundTrip(std::string_view FixtureName) -> void
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / FixtureName;
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests(std::format("/{}/", FixtureName), Root.generic_string() + "/");

		Durin::FPackagePath MeshPath;
		Durin::FPackagePath FirstMaterialPath;
		Durin::FPackagePath SecondMaterialPath;
		Durin::FPackagePath ComponentPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/{}/Mesh", FixtureName), MeshPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/{}/First", FixtureName), FirstMaterialPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/{}/Second", FixtureName), SecondMaterialPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(std::format("/{}/Component", FixtureName), ComponentPath));

		const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> MeshImport = Durin::AssetForge::Builtins::ImportStaticMeshForTest(Source.generic_string(), MeshPath.ToString());
		ASSERT_TRUE(MeshImport) << MeshImport.Message;
		Durin::DMaterial* First = nullptr;
		Durin::DMaterial* Second = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(FirstMaterialPath, First));
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SecondMaterialPath, Second));
		ASSERT_TRUE(Durin::SavePackage(First->GetPackage()));
		ASSERT_TRUE(Durin::SavePackage(Second->GetPackage()));

		TComponent* Component = nullptr;
		ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ComponentPath, Component));
		Component->SetStaticMesh(MeshImport.Asset);
		Component->SetMaterial(0, First);
		Component->SetMaterial(1, Second);
		ASSERT_TRUE(Durin::SavePackage(Component->GetPackage()));

		const auto ComponentData = Durin::FindAssetExact(ComponentPath);
		ASSERT_NE(ComponentData, nullptr);
		EXPECT_NE(std::ranges::find(ComponentData->Dependencies, MeshPath), ComponentData->Dependencies.end());
		EXPECT_NE(std::ranges::find(ComponentData->Dependencies, FirstMaterialPath), ComponentData->Dependencies.end());
		EXPECT_NE(std::ranges::find(ComponentData->Dependencies, SecondMaterialPath), ComponentData->Dependencies.end());

		const std::filesystem::path FixturePath = Durin::Testing::GetTestWorkDirectory()
			/ FixtureName / "Component.dasset";
		Durin::FByteBuffer FixtureBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FixtureBytes, FixturePath));
		Durin::ObjectPackage::FLinkerTables ComponentLinker;
		ASSERT_TRUE(Durin::ObjectPackage::ReadPackage(FixtureBytes, {}, ComponentPath, ComponentLinker));
		EXPECT_FALSE(ContainsSerializedField(ComponentLinker, "Materials"));
		EXPECT_FALSE(ContainsSerializedField(ComponentLinker, "MaterialOverridesVersion"));
		EXPECT_FALSE(ContainsSerializedField(ComponentLinker, "MaterialOverrides"));
		EXPECT_TRUE(ContainsSerializedField(ComponentLinker, "OverrideMaterials"));
		EXPECT_FALSE(ContainsSerializedField(ComponentLinker, "OverrideMaterials_DEPRECATED"));
		for (const auto& Export : ComponentLinker.Exports)
			for (const auto& Property : Export.Properties)
				if (Property.FieldName == "OverrideMaterials")
					EXPECT_EQ(Property.DeclaringType, "Durin::DMeshComponent");

		ASSERT_TRUE(Durin::UnloadPackage(ComponentPath));
		ASSERT_TRUE(Durin::UnloadPackage(SecondMaterialPath));
		ASSERT_TRUE(Durin::UnloadPackage(FirstMaterialPath));
		ASSERT_TRUE(Durin::UnloadPackage(MeshPath));

		TComponent* Loaded = nullptr;
		ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ComponentPath), Loaded));
		ASSERT_NE(Loaded, nullptr);
		EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
		ASSERT_NE(Loaded->GetStaticMesh(), nullptr);
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Loaded->GetStaticMesh());
		ASSERT_NE(Loaded->GetStaticMesh()->GetRenderData(), nullptr);
		EXPECT_EQ(Loaded->GetNumMaterials(), 2u);
		ASSERT_NE(Loaded->GetMaterial(0), nullptr);
		ASSERT_NE(Loaded->GetMaterial(1), nullptr);
		EXPECT_EQ(Loaded->GetMaterial(0)->GetPackage()->GetPackagePath(), FirstMaterialPath.ToString());
		EXPECT_EQ(Loaded->GetMaterial(1)->GetPackage()->GetPackagePath(), SecondMaterialPath.ToString());
		ASSERT_TRUE(Durin::SavePackage(Loaded->GetPackage()));
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FixtureBytes, FixturePath));
		ASSERT_TRUE(Durin::ObjectPackage::ReadPackage(FixtureBytes, {}, ComponentPath, ComponentLinker));
		EXPECT_FALSE(ContainsSerializedField(ComponentLinker, "OverrideMaterials_DEPRECATED"));
		for (const auto& Export : ComponentLinker.Exports)
			for (const auto& Property : Export.Properties)
				if (Property.FieldName == "OverrideMaterials")
					EXPECT_EQ(Property.DeclaringType, "Durin::DMeshComponent");

		ASSERT_TRUE(Durin::UnloadPackage(ComponentPath));
		ASSERT_TRUE(Durin::UnloadPackage(SecondMaterialPath));
		ASSERT_TRUE(Durin::UnloadPackage(FirstMaterialPath));
		ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	}
}

TEST(FStaticMeshMaterialTests, StaticMeshComponentOverridesRoundTripAfterMeshDependenciesLoad)
{
	VerifyComponentOverridesRoundTrip<Durin::DStaticMeshComponent>("StaticMeshSlotOverrides");
}

TEST(FStaticMeshMaterialTests, SplineMeshComponentOverridesRoundTripAfterMeshDependenciesLoad)
{
	VerifyComponentOverridesRoundTrip<Durin::DSplineMeshComponent>("SplineMeshSlotOverrides");
}

TEST(FStaticMeshMaterialTests, OverrideValidationRetainsCountsAndIncompatibleObjectContext)
{
	using namespace Durin;
	InitializeDObjectSystem();
	std::vector<TObjectPtr<DMaterialInterface>> Overrides(MaximumMeshMaterialSlots + 1);
	const auto Count = ValidateStaticMeshMaterialOverrides(Overrides);
	EXPECT_EQ(Count.Error.Code, EStaticMeshMaterialOverrideError::TooManySlots);
	Overrides.clear();
	EXPECT_EQ(Count.Error.ActualCount, MaximumMeshMaterialSlots + 1);
	EXPECT_EQ(Count.Error.MaximumCount, MaximumMeshMaterialSlots);
	ASSERT_TRUE(ValidateStaticMeshMaterialOverrides(Overrides));
	auto* Object = NewObject<DObject>(nullptr, "IncompatibleMaterialOverride");
	const auto Path = Object->GetObjectPath();
	const auto Type = Object->GetClass()->GetQualifiedName().ToString();
	Overrides.emplace_back(reinterpret_cast<DMaterialInterface*>(Object));
	const auto Rejected = ValidateStaticMeshMaterialOverrides(Overrides);
	EXPECT_EQ(Rejected.Error.Code, EStaticMeshMaterialOverrideError::IncompatibleObject);
	EXPECT_EQ(Rejected.Error.Index, 0u);
	EXPECT_EQ(Overrides.size(), 1u);
	Overrides.clear();
	MarkAsGarbage(Object);
	CollectGarbage();
	EXPECT_EQ(Rejected.Error.ObjectPath, Path);
	EXPECT_EQ(Rejected.Error.ActualType, Type);
}
