#include "StaticMeshMaterialTestFixture.h"

TEST(FStaticMeshImportTests, StaticMeshSourceProvenanceLivesOutsideContentAndSurvivesAssetOperations)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshSourceProvenance";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests(
		"/StaticMeshSourceProvenance/", (Root / "Content").generic_string() + "/");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/StaticMeshSourceProvenance/Environment/Mesh");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Import.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	const Durin::FSourceFile* ImportedSource =
		ImportData->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_TRUE(ImportedSource->Hint.ends_with("MultiSection.gltf"));
	EXPECT_EQ(ImportedSource->GetContentHash().ToString().size(), 32u);
	const std::string OriginalSourcePath = ImportedSource->Hint;
	const std::filesystem::path StoredSource = Source;
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Content/Models/Environment/Mesh.gltf"));

	Durin::FPackagePath OldPath;
	Durin::FPackagePath NewPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSourceProvenance/Environment/Mesh", OldPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/StaticMeshSourceProvenance/Moved/Mesh", NewPath));
	ASSERT_TRUE(RelocateAssetForTest(OldPath, NewPath));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
	ASSERT_NE(Import.Asset->GetAssetImportData(), nullptr);
	ImportedSource = Import.Asset->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, OriginalSourcePath);

	Durin::FAssetPackageInspection Inspection;
	const auto Data = Durin::FindAssetExact(NewPath);
	ASSERT_TRUE(Data);
	ASSERT_TRUE(Durin::InspectAssetPackage(Data->PhysicalPath, NewPath, Inspection));
	std::vector<std::filesystem::path> Companions;
	{
		auto ValueResult = Durin::InspectEditorBulkDataCompanionPaths(Data->PhysicalPath, Inspection);
		ASSERT_TRUE(ValueResult);
		Companions = std::move(*ValueResult);
	}
	EXPECT_TRUE(Companions.empty());
	ASSERT_TRUE(DeleteAssetClosureForTest({OldPath, NewPath}));
	EXPECT_TRUE(std::filesystem::is_regular_file(StoredSource));
}

TEST(FStaticMeshImportTests, StaticMeshImportSettingsValidateDistinctAxes)
{
	Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	EXPECT_TRUE(Settings.Validate());
	Settings.RightAxis = Durin::EStaticMeshImportAxis::PositiveZ;
	const auto Validation = Settings.Validate();
	EXPECT_FALSE(Validation);
	EXPECT_EQ(Validation.Error.Code, Durin::EStaticMeshImportSettingsError::RepeatedAxis);
	EXPECT_EQ(Validation.Error.RightAxis, Durin::EStaticMeshImportAxis::PositiveZ);
	Settings.UpAxis = static_cast<Durin::EStaticMeshImportAxis>(255);
	const auto Unknown = Settings.Validate();
	EXPECT_EQ(Unknown.Error.Code, Durin::EStaticMeshImportSettingsError::UnknownAxis);
	EXPECT_EQ(Unknown.Error.UpAxis, static_cast<Durin::EStaticMeshImportAxis>(255));
}

TEST(FStaticMeshImportTests, StaticMeshImportSettingsPersistAcrossSourceRebuild)
{
	InitializeDObjectSystem();
	const Durin::DClass* ImportDataClass =
		Durin::AssetForge::Builtins::DStaticMeshImportData::StaticClass();
	ASSERT_NE(ImportDataClass->FindPropertyByName("ImportSettings"), nullptr);
	EXPECT_EQ(ImportDataClass->FindPropertyByName("ImporterId"), nullptr);
	EXPECT_EQ(ImportDataClass->FindPropertyByName("ImporterVersion"), nullptr);
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshAxisImports";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests("/MeshAxisImportTests/", Root.generic_string() + "/");

	const Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "AsymmetricAxes.obj";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> ImportResult = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/MeshAxisImportTests/AsymmetricAxes", Settings);
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		ImportResult.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(ImportData->GetImportSettings(), Settings);
	ASSERT_NE(ImportResult.Asset->GetRenderData(), nullptr);
	ASSERT_EQ(ImportResult.Asset->GetRenderData()->LODResources.size(), 1u);
	const std::vector<Durin::FVector3f> ImportedPositions =
		ImportResult.Asset->GetRenderData()->LODResources[0]
			.VertexBuffers.PositionVertexBuffer.GetPositions();

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MeshAxisImportTests/AsymmetricAxes", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));

	Durin::DStaticMesh* Loaded = nullptr;
	{
		auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath));
		Loaded = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Loaded, nullptr);
	const auto* LoadedImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Loaded->GetAssetImportData());
	ASSERT_NE(LoadedImportData, nullptr);
	EXPECT_EQ(LoadedImportData->GetImportSettings(), Settings);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Loaded);
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
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
}
