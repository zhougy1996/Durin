#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "DObject/DurinPropertyTypes.h"
#include "EngineTestSupport.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

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

	auto CopyLegacyMigrationFixture(
		const std::filesystem::path& FixtureRoot,
		const std::filesystem::path& WorkRoot,
		std::string_view FileName,
		std::string_view Owner,
		std::string_view AssetName) -> void
	{
		const std::filesystem::path ContentRoot = WorkRoot / Owner / "Content";
		std::filesystem::create_directories(ContentRoot);
		std::filesystem::copy_file(
			FixtureRoot / FileName,
			ContentRoot / (std::string(AssetName) + ".dasset"),
			std::filesystem::copy_options::overwrite_existing);
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

TEST(FSourcePathContractTests, SharedSourceOperationsClassifyIngestAndRollback)
{
	const std::filesystem::path Root =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "MountedSourceOperations";
	std::filesystem::remove_all(Root);
	const std::filesystem::path EngineSource =
		Root / "Engine" / "SourceAssets" / "Textures" / "Shared.bin";
	const std::filesystem::path ExternalSource = Root / "External" / "Input.bin";
	std::filesystem::create_directories(EngineSource.parent_path());
	std::filesystem::create_directories(ExternalSource.parent_path());
	std::filesystem::create_directories(Root / "Game" / "SourceAssets");
	std::filesystem::create_directories(Root / "Game" / "Content");
	{
		std::ofstream Stream(EngineSource, std::ios::binary);
		Stream << "engine-bytes";
	}
	{
		std::ofstream Stream(ExternalSource, std::ios::binary);
		Stream << "external-bytes";
	}
	const std::array Mounts = {
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Engine,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine" / "Content",
			.SourceAssetsRoot = Root / "Engine" / "SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.OwnerRoot = Root / "Game",
			.ContentRoot = Root / "Game" / "Content",
			.SourceAssetsRoot = Root / "Game" / "SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	Durin::FMountedSourceFile Prepared;
	std::string Error;
	ASSERT_TRUE(Durin::PrepareMountedSourceFile(
		EngineSource, "/Game/Textures/Asset", "/Game/Unused.bin", Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.SourcePath.Path, "/Engine/Textures/Shared.bin");
	EXPECT_EQ(Prepared.Disposition, Durin::ESourceFileDisposition::ReferenceExisting);
	EXPECT_FALSE(Prepared.bCreatedFile);
	EXPECT_FALSE(std::filesystem::exists(Root / "Game" / "SourceAssets" / "Unused.bin"));

	ASSERT_TRUE(Durin::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.SourcePath.Path, "/Game/Textures/Ingested.bin");
	EXPECT_EQ(Prepared.Disposition, Durin::ESourceFileDisposition::IngestedExternal);
	ASSERT_TRUE(std::filesystem::is_regular_file(Prepared.PhysicalPath));
	Durin::RollbackMountedSourceFile(Prepared);
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Game" / "SourceAssets" / "Textures" / "Ingested.bin"));

	ASSERT_TRUE(Durin::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	Durin::CommitMountedSourceFile(Prepared);
	ASSERT_TRUE(Durin::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Other", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.Disposition, Durin::ESourceFileDisposition::ReusedIdentical);

	Durin::FMountedSourceReplacement Replacement;
	EXPECT_FALSE(Durin::PrepareMountedSourceReplacement(
		ExternalSource, "/Game/Textures/Asset", "/Engine/Textures/Shared.bin",
		Replacement, Error));
	ASSERT_TRUE(Durin::PrepareMountedSourceReplacement(
		EngineSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Replacement, Error)) << Error;
	EXPECT_TRUE(Replacement.bPublished);
	Durin::RollbackMountedSourceReplacement(Replacement);
	std::ifstream Restored(
		Root / "Game" / "SourceAssets" / "Textures" / "Ingested.bin",
		std::ios::binary);
	std::string RestoredBytes(
		(std::istreambuf_iterator<char>(Restored)), std::istreambuf_iterator<char>());
	EXPECT_EQ(RestoredBytes, "external-bytes");
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

TEST(FSourcePathContractTests, LegacyFixturesAreRejectedAfterCarrierRetirement)
{
	InitializeDObjectSystem();
	const std::filesystem::path FixtureRoot =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "SourceLibraryReferences";
	const std::filesystem::path WorkRoot =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "SourcePathMigration";
	std::filesystem::remove_all(WorkRoot);
	std::filesystem::create_directories(WorkRoot / "Project");
	std::filesystem::create_directories(WorkRoot / "Engine");
	std::filesystem::copy(
		FixtureRoot / "Project" / "SourceAssets",
		WorkRoot / "Project" / "SourceAssets",
		std::filesystem::copy_options::recursive);
	std::filesystem::copy(
		FixtureRoot / "Engine" / "SourceAssets",
		WorkRoot / "Engine" / "SourceAssets",
		std::filesystem::copy_options::recursive);
	CopyLegacyMigrationFixture(
		FixtureRoot, WorkRoot, "LegacyProjectStaticMesh.dasset", "Project", "LegacyProjectStaticMesh");
	CopyLegacyMigrationFixture(
		FixtureRoot, WorkRoot, "LegacyEngineStaticMesh.dasset", "Engine", "LegacyEngineStaticMesh");
	CopyLegacyMigrationFixture(
		FixtureRoot, WorkRoot, "LegacyProjectTexture2D.dasset", "Project", "LegacyProjectTexture2D");
	CopyLegacyMigrationFixture(
		FixtureRoot, WorkRoot, "LegacyProjectTextureCubeSixFaces.dasset", "Project", "LegacyProjectTextureCubeSixFaces");
	CopyLegacyMigrationFixture(
		FixtureRoot, WorkRoot, "LegacyProjectTextureCubePanorama.dasset", "Project", "LegacyProjectTextureCubePanorama");

	const std::array Mounts = {
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Engine,
			.OwnerRoot = WorkRoot / "Engine",
			.ContentRoot = WorkRoot / "Engine" / "Content",
			.SourceAssetsRoot = WorkRoot / "Engine" / "SourceAssets",
			.Dependencies = {}},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.OwnerRoot = WorkRoot / "Project",
			.ContentRoot = WorkRoot / "Project" / "Content",
			.SourceAssetsRoot = WorkRoot / "Project" / "SourceAssets",
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	for (std::string_view AssetPath : {
		"/Game/LegacyProjectStaticMesh",
		"/Engine/LegacyEngineStaticMesh",
		"/Game/LegacyProjectTexture2D",
		"/Game/LegacyProjectTextureCubeSixFaces",
		"/Game/LegacyProjectTextureCubePanorama"})
	{
		SCOPED_TRACE(AssetPath);
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(AssetPath, Path));
		Durin::DObject* Object = nullptr;
		const Durin::Asset::FAssetResult LoadResult =
			Durin::Asset::LoadAsset(Path, Object);
		EXPECT_EQ(LoadResult.Error, Durin::Asset::EAssetError::TypeMismatch);
		EXPECT_NE(LoadResult.Message.find("SourcePath"), std::string::npos);
		EXPECT_EQ(Object, nullptr);
	}
}
