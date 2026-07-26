#include <gtest/gtest.h>

#include "AssetSystem.h"

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
}

TEST(FSourceLibraryReferenceContractTests, LegacyFixturesPreserveVersionTwoSourceLayouts)
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

TEST(FSourceLibraryReferenceContractTests, LegacyFixturesRetainExplicitSourcePathCarriers)
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
