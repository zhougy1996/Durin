#include "MaterialGraphDocument.h"
#include "MaterialGraphTestSupport.h"

TEST(FMaterialGraphPersistenceTests, CustomDeclarationsPersistAndDuplicateTheirIdentity)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "CustomDeclarations";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/CustomDeclarations/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CustomDeclarations/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0.4});
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Parameter.Get()}, {}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCustomDeclarations").Object);
	ASSERT_NE(Duplicate, nullptr);
	ASSERT_EQ(Duplicate->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Duplicate->GetParameterDefinitions().front(), Definition);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material));
	ASSERT_NE(Material, nullptr);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front(), Definition);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

TEST(FMaterialGraphPersistenceTests, LargeGraphRoundTripsWithoutLoadedOverrideLedger)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "GraphWithoutLedger";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/GraphWithoutLedger/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphWithoutLedger/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Testing::FTestMaterialExpressionGraph Graph;
	for (int Index = 0; Index < 65; ++Index)
	{
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = std::format("Tint{}", Index);
		Definition.Type = EMaterialParameterType::Vector4;
		Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.0, 0.3, 1.0});
		auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
		Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
		Parameter->DefaultValue = Definition.Value.GetVector4();
		Graph.Expressions.emplace_back(Parameter.Get());
	}
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage(), EAssetPackageSaveMode::Complete));
	const auto CompleteBytes = std::filesystem::file_size(Root / "Base.dasset");
	for (int Round = 0; Round < 2; ++Round)
	{
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		const auto DeltaBytes = std::filesystem::file_size(Root / "Base.dasset");
		EXPECT_LE(DeltaBytes, CompleteBytes);
		ASSERT_TRUE(UnloadPackage(Path));
		const auto LoadResult = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(CaptureExpressions(*Material), Expected);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy").Object);
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Copy->HasAllocatedAuthoredOverrideLedger());
		EXPECT_EQ(CaptureExpressions(*Copy), Expected);
		MarkObjectHierarchyAsGarbage(Copy);
	}
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

TEST(FMaterialAssetCreationPersistenceTests, BuiltInMaterialsHaveCompletePersistentGraphPresentation)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	const FAssetCatalogRefreshResult Refresh =
		RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Refresh) << (Refresh.Errors.empty()
		? "Asset catalog refresh failed without a diagnostic."
		: Durin::FormatAssetRegistryError(Refresh.Errors.front()));

	for (const std::string_view PathString : {
		"/Engine/Materials/DefaultMaterial"})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(PathString, Path));
		DMaterial* Material = nullptr;
		const auto Loaded = LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_NE(Material, nullptr);
		const FMaterialGraphPresentation& Presentation =
			Material->GetMaterialGraphPresentation();
		EXPECT_EQ(Presentation.Nodes.size(),
			Material->GetExpressionCollection().Expressions.size());
		EXPECT_NE(Material->GetOutputNode(), nullptr);
		const FMaterialGraphView View = FMaterialGraphDocument(*Material).Inspect();
		EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
		ASSERT_TRUE(UnloadPackage(Path));
	}
	CollectGarbage();
}
