#include "MaterialGraphDocument.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "DObject/OverridableManager.h"
#include "MaterialGraphTestSupport.h"

TEST(FMaterialGraphPersistenceTests, TextureCoordinatesRetainChannelLinkAndDefaultAcrossPackageRoundTrip)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "TextureCoordinatesRoundTrip";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/TextureCoordinatesRoundTrip/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TextureCoordinatesRoundTrip/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	auto Channel = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Channel->Value = 1.f;
	auto Coordinates = Testing::MakeGraphExpression<DMaterialExpressionTextureCoordinates>();
	Coordinates->Channel.Constant = {2.f};
	Coordinates->Channel.Connection = {Channel->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(
		std::array<DMaterialExpression*, 2>{Channel.Get(), Coordinates.Get()}, {}));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCoordinates").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(CaptureExpressions(*Duplicate), Expected);
	MarkObjectHierarchyAsGarbage(Duplicate);
	// Release transient source expressions before unloading their duplicated package graph.
	Channel.Reset();
	Coordinates.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		const auto Loaded = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		ASSERT_TRUE(Loaded);
		Material = Loaded;
	}
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Expected);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

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
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCustomDeclarations").value());
	ASSERT_NE(Duplicate, nullptr);
	ASSERT_EQ(Duplicate->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Duplicate->GetParameterDefinitions().front(), Definition);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Material = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Material, nullptr);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front(), Definition);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	ASSERT_TRUE(SavePackage(Material->GetPackage(), {.Mode = EAssetPackageSaveMode::Complete}));
	const auto CompleteBytes = std::filesystem::file_size(Root / "Base.dasset");
	for (int Round = 0; Round < 2; ++Round)
	{
		std::optional<Durin::UnifiedError::FError> LoadResultError;
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		const auto DeltaBytes = std::filesystem::file_size(Root / "Base.dasset");
		EXPECT_LE(DeltaBytes, CompleteBytes);
		ASSERT_TRUE(UnloadPackage(Path));
		const auto LoadResult = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadResultError);
		Material = LoadResult;
		ASSERT_TRUE(LoadResult) << (LoadResultError ? LoadResultError->ToString() : "");
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Durin::FOverridableManager::HasOverriddenProperties(*Material));
		EXPECT_EQ(CaptureExpressions(*Material), Expected);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy").value());
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Durin::FOverridableManager::HasOverriddenProperties(*Copy));
		EXPECT_EQ(CaptureExpressions(*Copy), Expected);
		MarkObjectHierarchyAsGarbage(Copy);
	}
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
		std::optional<Durin::UnifiedError::FError> LoadedError;
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(PathString, Path));
		DMaterial* Material = nullptr;
		const auto Loaded = LoadObject<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		Material = Loaded;
		ASSERT_TRUE(Loaded) << (LoadedError ? LoadedError->ToString() : "");
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
}


TEST(FMaterialGraphPersistenceTests, TextureSamplingModeAndInactiveOperandsRoundTrip)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "TextureSamplingRoundTrip";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/TextureSamplingRoundTrip/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TextureSamplingRoundTrip/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "Sample"};
	Sample->ReadMode = EMaterialTextureReadMode::Level;
	Sample->MipValue.SetConstant({2.f});
	Sample->DDX.SetConstant({.125f,.25f});
	Sample->DDY.SetConstant({.5f,.75f});
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Sample.Get()}, {}));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Sample.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Expected);
	const auto It = std::ranges::find_if(Material->GetExpressionCollection().Expressions,
		[](const auto& E) { return Cast<DMaterialExpressionTextureSampleParameter2D>(E.Get()) != nullptr; });
	ASSERT_NE(It, Material->GetExpressionCollection().Expressions.end());
	const auto* LoadedSample = Cast<DMaterialExpressionTextureSampleParameter2D>(It->Get());
	EXPECT_EQ(LoadedSample->ReadMode, EMaterialTextureReadMode::Level);
	EXPECT_EQ(LoadedSample->DDX.Constant, (std::vector<float>{.125f,.25f}));
	EXPECT_EQ(LoadedSample->DDY.Constant, (std::vector<float>{.5f,.75f}));
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}
