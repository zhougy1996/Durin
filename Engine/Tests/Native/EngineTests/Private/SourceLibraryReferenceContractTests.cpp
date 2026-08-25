#include <gtest/gtest.h>

#include "AssetTools.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "EngineTestSupport.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace
{
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
	ASSERT_NE(SourcePathStruct->GetOuter(), nullptr);
	EXPECT_EQ(SourcePathStruct->GetOuter()->GetName(), "AssetCore");

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

TEST(FSourcePathContractTests, TextureLeafIdentityAndPropertyDeclarationsRemainStable)
{
	static_assert(std::is_base_of_v<Durin::DObject, Durin::DTexture2D>);
	static_assert(std::is_base_of_v<Durin::DObject, Durin::DTextureCube>);
	static_assert(std::is_base_of_v<Durin::DObject, Durin::DTexture>);
	static_assert(std::is_base_of_v<Durin::DTexture, Durin::DTexture2D>);
	static_assert(std::is_base_of_v<Durin::DTexture, Durin::DTextureCube>);

	InitializeDObjectSystem();
	Durin::DClass* TextureClass = Durin::DTexture::StaticClass();
	Durin::DClass* Texture2DClass = Durin::DTexture2D::StaticClass();
	Durin::DClass* TextureCubeClass = Durin::DTextureCube::StaticClass();
	ASSERT_NE(TextureClass, nullptr);
	ASSERT_NE(Texture2DClass, nullptr);
	ASSERT_NE(TextureCubeClass, nullptr);
	EXPECT_EQ(TextureClass->GetQualifiedName().ToString(), "Durin::DTexture");
	EXPECT_EQ(Texture2DClass->GetQualifiedName().ToString(), "Durin::DTexture2D");
	EXPECT_EQ(TextureCubeClass->GetQualifiedName().ToString(), "Durin::DTextureCube");
	EXPECT_EQ(TextureClass->GetSuperClass(), Durin::DObject::StaticClass());
	EXPECT_EQ(Texture2DClass->GetSuperClass(), TextureClass);
	EXPECT_EQ(TextureCubeClass->GetSuperClass(), TextureClass);
	EXPECT_TRUE(TextureClass->HasAnyClassFlags(Durin::EClassFlags::Abstract));
	EXPECT_EQ(TextureClass->ClassConstructor, nullptr);
	EXPECT_FALSE(Durin::CanConstructObjectOfClass(
		TextureClass, Durin::DObject::StaticClass()));
	EXPECT_EQ(Durin::NewObject(
		TextureClass, nullptr, Durin::FName("RejectedTexture")), nullptr);
	Durin::DObject* Texture2DObject = Durin::NewObject(
		Texture2DClass, nullptr, Durin::FName("TextureHierarchy2D"));
	Durin::DObject* TextureCubeObject = Durin::NewObject(
		TextureCubeClass, nullptr, Durin::FName("TextureHierarchyCube"));
	ASSERT_NE(Texture2DObject, nullptr);
	ASSERT_NE(TextureCubeObject, nullptr);
	EXPECT_EQ(Durin::Cast<Durin::DTexture>(Texture2DObject), Texture2DObject);
	EXPECT_EQ(Durin::Cast<Durin::DTexture>(TextureCubeObject), TextureCubeObject);

	static constexpr std::array Texture2DProperties = {
		std::string_view("SourceImportData"),
		std::string_view("SourceContentHash"),
		std::string_view("SourceFileSize"),
		std::string_view("SourceLastWriteTime"),
		std::string_view("SourceWidth"),
		std::string_view("SourceHeight"),
		std::string_view("SourceChannelCount"),
		std::string_view("bSourceHasTransparency"),
		std::string_view("Usage"),
		std::string_view("bSRGB"),
		std::string_view("MaxResolution"),
		std::string_view("CompressionQuality"),
		std::string_view("AlphaMipMode"),
		std::string_view("AlphaCoverageThreshold"),
		std::string_view("CookedPayload")};
	static constexpr std::array TextureCubeProperties = {
		std::string_view("SourceLayout"),
		std::string_view("SourceImportData"),
		std::string_view("PanoramaFaceDimension"),
		std::string_view("PanoramaExposureEV"),
		std::string_view("OriginalSourceWidth"),
		std::string_view("OriginalSourceHeight"),
		std::string_view("bSRGB"),
		std::string_view("CookedPayload")};

	const auto ExpectDeclaredProperties = [](Durin::DClass* Class,
		std::span<const std::string_view> PropertyNames) {
		for (std::string_view PropertyName : PropertyNames)
		{
			SCOPED_TRACE(PropertyName);
			Durin::FProperty* Property =
				Class->FindPropertyByName(Durin::FName(PropertyName));
			ASSERT_NE(Property, nullptr);
			EXPECT_EQ(Property->Owner.ToDObject(), Class);
		}
	};
	ExpectDeclaredProperties(Texture2DClass, Texture2DProperties);
	ExpectDeclaredProperties(TextureCubeClass, TextureCubeProperties);
	EXPECT_EQ(Durin::DStaticMesh::StaticClass()->FindPropertyByName("SourceFile"), nullptr);
	EXPECT_EQ(Durin::DStaticMesh::StaticClass()->FindPropertyByName("ImportSettings"), nullptr);
	EXPECT_EQ(Texture2DClass->FindPropertyByName("SourceFile"), nullptr);
	for (std::string_view RetiredField : {
		"PositiveXSourceFile",
		"NegativeXSourceFile",
		"PositiveYSourceFile",
		"NegativeYSourceFile",
		"PositiveZSourceFile",
		"NegativeZSourceFile",
		"PanoramaSourceFile"})
		EXPECT_EQ(TextureCubeClass->FindPropertyByName(RetiredField), nullptr);
}

TEST(FSourcePathContractTests, UnifiedMountFixtureFreezesSingleRootsCapabilitiesAndDependencyCases)
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
	EXPECT_EQ(Plugin.GetView("ContentPath").GetString(), "Content");
	EXPECT_TRUE(Plugin.GetView("AutoScan").GetBool());
	EXPECT_FALSE(Plugin.GetView("AuthoringWritable").GetBool());

	const Durin::FJsonNodeView External = FindNamedEntry(Mounts, "/Libraries/StudioArt/");
	ASSERT_TRUE(External.IsObject());
	EXPECT_EQ(External.GetView("Owner").GetString(), "ExternalSources");
	EXPECT_EQ(External.GetView("Root").GetString(), "StudioArt");
	EXPECT_EQ(External.GetView("ContentPath").GetString(), ".");
	EXPECT_FALSE(External.GetView("AutoScan").GetBool());
	EXPECT_FALSE(External.GetView("AuthoringWritable").GetBool());

	const Durin::FJsonNodeView Cases = Contract.GetRootView().GetView("Cases");
	ASSERT_TRUE(Cases.IsArray());
	ASSERT_EQ(Cases.Num(), 5u);
	EXPECT_EQ(FindNamedEntry(Cases, "GameToEngineSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "EngineToGameSource").GetView("ExpectedError").GetString(), "ForbiddenDependency");
	EXPECT_EQ(FindNamedEntry(Cases, "GameToPluginSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "ManualScanAsset").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "ManualScanSource").GetView("ExpectedError").GetString(), "None");
}

TEST(FSourcePathContractTests, SharedSourceOperationsClassifyIngestAndRollback)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "MountedSourceOperations";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path EngineSource =
		Root / "Engine" / "Content" / "Textures" / "Shared.bin";
	const std::filesystem::path ExternalSource = Root / "External" / "Input.bin";
	std::filesystem::create_directories(EngineSource.parent_path());
	std::filesystem::create_directories(ExternalSource.parent_path());
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
			.Root = Root / "Engine",
			.ContentPath = "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.Root = Root / "Game",
			.ContentPath = "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	Durin::Asset::FMountedSourceFile Prepared;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		EngineSource, "/Game/Textures/Asset", "/Game/Unused.bin", Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.SourcePath.Path, "/Engine/Textures/Shared.bin");
	EXPECT_EQ(Prepared.Disposition, Durin::Asset::ESourceFileDisposition::ReferenceExisting);
	EXPECT_FALSE(Prepared.bCreatedFile);
	EXPECT_FALSE(std::filesystem::exists(Root / "Game" / "Content" / "Unused.bin"));

	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.SourcePath.Path, "/Game/Textures/Ingested.bin");
	EXPECT_EQ(Prepared.Disposition, Durin::Asset::ESourceFileDisposition::IngestedExternal);
	ASSERT_TRUE(std::filesystem::is_regular_file(Prepared.PhysicalPath));
	Durin::Asset::RollbackMountedSourceFile(Prepared);
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Game" / "Content" / "Textures" / "Ingested.bin"));

	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	Durin::Asset::CommitMountedSourceFile(Prepared);
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Other", "/Game/Textures/Ingested.bin",
		Prepared, Error)) << Error;
	EXPECT_EQ(Prepared.Disposition, Durin::Asset::ESourceFileDisposition::ReusedIdentical);

	EXPECT_FALSE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Engine/Models/Asset", "/Engine/Models/Ingested.bin",
		Prepared, Error));
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Engine/Models/Asset", "/Engine/Models/Ingested.bin",
		Prepared, Error,
		Durin::Asset::EMountedSourceMutationContext::EngineAuthoring)) << Error;
	EXPECT_EQ(Prepared.SourcePath.Path, "/Engine/Models/Ingested.bin");
	EXPECT_EQ(Prepared.Disposition, Durin::Asset::ESourceFileDisposition::IngestedExternal);
	Durin::Asset::RollbackMountedSourceFile(Prepared);
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Engine" / "Content" / "Models" / "Ingested.bin"));

	Durin::Asset::FMountedSourceReplacement Replacement;
	EXPECT_FALSE(Durin::Asset::PrepareMountedSourceReplacement(
		ExternalSource, "/Game/Textures/Asset", "/Engine/Textures/Shared.bin",
		Replacement, Error));
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceReplacement(
		EngineSource, "/Game/Textures/Asset", "/Game/Textures/Ingested.bin",
		Replacement, Error)) << Error;
	EXPECT_TRUE(Replacement.bPublished);
	Durin::Asset::RollbackMountedSourceReplacement(Replacement);
	std::ifstream Restored(
		Root / "Game" / "Content" / "Textures" / "Ingested.bin",
		std::ios::binary);
	std::string RestoredBytes(
		(std::istreambuf_iterator<char>(Restored)), std::istreambuf_iterator<char>());
	EXPECT_EQ(RestoredBytes, "external-bytes");
}
