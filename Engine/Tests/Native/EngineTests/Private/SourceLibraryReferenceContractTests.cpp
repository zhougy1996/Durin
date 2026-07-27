#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "DObject/DurinPropertyTypes.h"
#include "Json/Json.h"
#include "Source/SourcePath.h"

namespace
{
	struct FLegacySourceFixture
	{
		std::string_view FileName;
		std::string_view AssetClass;
		std::string_view OwnerRoot;
		std::span<const std::string_view> TopLevelFields;
		std::span<const std::string_view> SourcePaths;
	};

	auto PayloadContainsText(const Durin::Asset::FAssetPackageField& Field, std::string_view Text) -> bool
	{
		return std::search(Field.Payload.begin(), Field.Payload.end(), Text.begin(), Text.end())
			!= Field.Payload.end();
	}

	auto InspectFixture(std::string_view FileName) -> Durin::Asset::FAssetPackageInspection
	{
		const std::filesystem::path Path =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / "SourceLibraryReferences" / FileName;
		Durin::Asset::FAssetPackageInspection Inspection;
		EXPECT_TRUE(Durin::Asset::InspectAssetPackage(Path.generic_string(), Inspection)) << Path;
		return Inspection;
	}

	auto FindNamedEntry(Durin::FJsonNodeView Entries, std::string_view Name) -> Durin::FJsonNodeView
	{
		for (size_t Index = 0; Index < Entries.Num(); ++Index)
		{
			const Durin::FJsonNodeView Entry = Entries.GetView(Index);
			if (Entry.GetView("Name").GetString() == Name) return Entry;
			if (Entry.GetView("VirtualRoot").GetString() == Name) return Entry;
		}
		return {};
	}
}

TEST(FSourcePathContractTests, ReflectedValueHasOneCompleteVirtualPath)
{
	Durin::DStruct* SourcePathStruct = Durin::FSourcePath::StaticStruct();
	ASSERT_NE(SourcePathStruct, nullptr);
	EXPECT_EQ(SourcePathStruct->GetQualifiedName().ToString(), "Durin::FSourcePath");

	size_t PropertyCount = 0;
	SourcePathStruct->ForEachProperty([&PropertyCount](Durin::FProperty*) { ++PropertyCount; });
	EXPECT_EQ(PropertyCount, 1u);
	Durin::FProperty* PathProperty = SourcePathStruct->FindPropertyByName(Durin::FName("Path"));
	ASSERT_NE(PathProperty, nullptr);
	EXPECT_EQ(PathProperty->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
	EXPECT_EQ(SourcePathStruct->FindPropertyByName(Durin::FName("Library")), nullptr);
	EXPECT_EQ(SourcePathStruct->FindPropertyByName(Durin::FName("RelativePath")), nullptr);

	Durin::FSourcePath Empty;
	EXPECT_TRUE(Empty.IsEmpty());
	const Durin::FSourcePath EngineSource{.Path = "/Engine/Textures/Common/Stone.png"};
	EXPECT_FALSE(EngineSource.IsEmpty());
	EXPECT_EQ(EngineSource.Path, "/Engine/Textures/Common/Stone.png");
}

TEST(FSourcePathContractTests, UnifiedMountFixtureFreezesDomainsAndDependencyCases)
{
	const std::filesystem::path Path =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "SourceLibraryReferences" / "UnifiedMountContract.json";
	Durin::FJsonDocument Contract;
	Durin::FJsonParseError ParseError;
	ASSERT_TRUE(Contract.LoadFromFile(Path.generic_string(), &ParseError)) << ParseError.Message;

	const Durin::FJsonNodeView Mounts = Contract.GetRootView().GetView("Mounts");
	ASSERT_TRUE(Mounts.IsArray());
	ASSERT_EQ(Mounts.Num(), 2u);

	const Durin::FJsonNodeView Plugin = FindNamedEntry(Mounts, "/Plugins/PCG/");
	ASSERT_TRUE(Plugin.IsObject());
	EXPECT_EQ(Plugin.GetView("Owner").GetString(), "Extension");
	EXPECT_EQ(Plugin.GetView("Root").GetString(), "Plugin");
	EXPECT_EQ(Plugin.GetView("Domains").GetView("Content").GetString(), "Content");
	EXPECT_EQ(Plugin.GetView("Domains").GetView("SourceAssets").GetString(), "SourceAssets");
	EXPECT_TRUE(Plugin.GetView("SourceWritable").IsBool());
	EXPECT_FALSE(Plugin.GetView("SourceWritable").GetBool());

	const Durin::FJsonNodeView SourceOnly = FindNamedEntry(Mounts, "/Libraries/StudioArt/");
	ASSERT_TRUE(SourceOnly.IsObject());
	EXPECT_EQ(SourceOnly.GetView("Owner").GetString(), "ExternalSources");
	EXPECT_FALSE(SourceOnly.GetView("Domains").Contains("Content"));
	EXPECT_EQ(SourceOnly.GetView("Domains").GetView("SourceAssets").GetString(), ".");

	const Durin::FJsonNodeView Cases = Contract.GetRootView().GetView("Cases");
	ASSERT_TRUE(Cases.IsArray());
	ASSERT_EQ(Cases.Num(), 5u);
	EXPECT_EQ(FindNamedEntry(Cases, "GameToEngineSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "EngineToGameSource").GetView("ExpectedError").GetString(), "ForbiddenDependency");
	EXPECT_EQ(FindNamedEntry(Cases, "GameToPluginSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "SourceOnlyContent").GetView("ExpectedError").GetString(), "UnsupportedDomain");
	EXPECT_EQ(FindNamedEntry(Cases, "SourceOnlySource").GetView("ExpectedError").GetString(), "None");
}

TEST(FSourcePathContractTests, LegacyFixturesPreserveVersionTwoSourceLayouts)
{
	static constexpr std::array StaticMeshFields = {
		std::string_view("SourceFile"),
		std::string_view("SourceImportData")};
	static constexpr std::array Texture2DFields = {
		std::string_view("SourceFile"),
		std::string_view("SourceImportData")};
	static constexpr std::array TextureCubeFields = {
		std::string_view("SourceImportData"),
		std::string_view("PositiveXSourceFile"),
		std::string_view("NegativeXSourceFile"),
		std::string_view("PositiveYSourceFile"),
		std::string_view("NegativeYSourceFile"),
		std::string_view("PositiveZSourceFile"),
		std::string_view("NegativeZSourceFile")};
	static constexpr std::array ProjectStaticMeshSources = {
		std::string_view("SourceAssets/Models/Models/Mesh_Teapot.obj")};
	static constexpr std::array EngineStaticMeshSources = {
		std::string_view("SourceAssets/Models/Editor/MaterialPreview/Sphere.obj")};
	static constexpr std::array Texture2DSources = {
		std::string_view("SourceAssets/Textures/Textures/TEX_StoneHead.jpg")};
	static constexpr std::array TextureCubeSixFaceSources = {
		std::string_view("SourceAssets/Textures/Convention_px.png"),
		std::string_view("SourceAssets/Textures/Convention_nx.png"),
		std::string_view("SourceAssets/Textures/Convention_py.png"),
		std::string_view("SourceAssets/Textures/Convention_ny.png"),
		std::string_view("SourceAssets/Textures/Convention_pz.png"),
		std::string_view("SourceAssets/Textures/Convention_nz.png")};
	static constexpr std::array TextureCubePanoramaSources = {
		std::string_view("SourceAssets/Textures/Panorama_panorama.tga")};
	static constexpr std::array Fixtures = {
		FLegacySourceFixture{"LegacyProjectStaticMesh.dasset", "Durin::DStaticMesh",
			"Project", StaticMeshFields, ProjectStaticMeshSources},
		FLegacySourceFixture{"LegacyEngineStaticMesh.dasset", "Durin::DStaticMesh",
			"Engine", StaticMeshFields, EngineStaticMeshSources},
		FLegacySourceFixture{"LegacyProjectTexture2D.dasset", "Durin::DTexture2D",
			"Project", Texture2DFields, Texture2DSources},
		FLegacySourceFixture{"LegacyProjectTextureCubeSixFaces.dasset", "Durin::DTextureCube",
			"Project", TextureCubeFields, TextureCubeSixFaceSources},
		FLegacySourceFixture{"LegacyProjectTextureCubePanorama.dasset", "Durin::DTextureCube",
			"Project", TextureCubeFields, TextureCubePanoramaSources}};

	for (const FLegacySourceFixture& Fixture : Fixtures)
	{
		SCOPED_TRACE(Fixture.FileName);
		const Durin::Asset::FAssetPackageInspection Inspection = InspectFixture(Fixture.FileName);
		EXPECT_EQ(Inspection.Header.FormatVersion, 2u);
		EXPECT_EQ(Inspection.Header.AssetClassName, Fixture.AssetClass);
		for (std::string_view Field : Fixture.TopLevelFields)
			EXPECT_NE(Inspection.FindField(Field), nullptr) << Field;
		const Durin::Asset::FAssetPackageField* SourceImportData =
			Inspection.FindField("SourceImportData");
		ASSERT_NE(SourceImportData, nullptr);
		for (std::string_view SourcePath : Fixture.SourcePaths)
		{
			EXPECT_TRUE(PayloadContainsText(*SourceImportData, SourcePath)) << SourcePath;
			EXPECT_TRUE(std::filesystem::is_regular_file(
				std::filesystem::path(DURIN_TEST_DATA_DIR) / "SourceLibraryReferences"
				/ Fixture.OwnerRoot / SourcePath)) << SourcePath;
		}
	}
}

TEST(FSourcePathContractTests, LegacyFixturesRetainExplicitSourcePathCarriers)
{
	for (std::string_view FileName : {
		"LegacyProjectStaticMesh.dasset",
		"LegacyEngineStaticMesh.dasset",
		"LegacyProjectTexture2D.dasset",
		"LegacyProjectTextureCubeSixFaces.dasset",
		"LegacyProjectTextureCubePanorama.dasset"})
	{
		SCOPED_TRACE(FileName);
		const Durin::Asset::FAssetPackageInspection Inspection = InspectFixture(FileName);
		const Durin::Asset::FAssetPackageField* SourceImportData =
			Inspection.FindField("SourceImportData");
		ASSERT_NE(SourceImportData, nullptr);
		EXPECT_TRUE(PayloadContainsText(*SourceImportData, "SourcePath"));
		EXPECT_TRUE(PayloadContainsText(*SourceImportData, "SourceContentHash"));
	}
}
