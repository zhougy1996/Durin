#include <gtest/gtest.h>

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
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
TEST(FSourceFileContractTests, TextureLeafIdentityAndPropertyDeclarationsRemainStable)
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
		std::string_view("AssetImportData"),
		std::string_view("SourceWidth"),
		std::string_view("SourceHeight"),
		std::string_view("SourceChannelCount"),
		std::string_view("bSourceHasTransparency"),
		std::string_view("Usage"),
		std::string_view("bSRGB"),
		std::string_view("MaxResolution"),
		std::string_view("CompressionQuality"),
		std::string_view("AlphaMipMode"),
		std::string_view("AlphaCoverageThreshold")};
	static constexpr std::array TextureCubeProperties = {
		std::string_view("SourceLayout"),
		std::string_view("AssetImportData"),
		std::string_view("PanoramaFaceDimension"),
		std::string_view("PanoramaExposureEV"),
		std::string_view("OriginalSourceWidth"),
		std::string_view("OriginalSourceHeight"),
		std::string_view("bSRGB")};

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

TEST(FSourceFileContractTests, UnifiedMountFixtureFreezesSingleRootsCapabilitiesAndDependencyCases)
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
	EXPECT_FALSE(Plugin.GetView("ContentWritable").GetBool());

	const Durin::FJsonNodeView External = FindNamedEntry(Mounts, "/Libraries/StudioArt/");
	ASSERT_TRUE(External.IsObject());
	EXPECT_EQ(External.GetView("Owner").GetString(), "ExternalSources");
	EXPECT_EQ(External.GetView("Root").GetString(), "StudioArt");
	EXPECT_EQ(External.GetView("ContentPath").GetString(), ".");
	EXPECT_FALSE(External.GetView("AutoScan").GetBool());
	EXPECT_FALSE(External.GetView("ContentWritable").GetBool());

	const Durin::FJsonNodeView Cases = Contract.GetRootView().GetView("Cases");
	ASSERT_TRUE(Cases.IsArray());
	ASSERT_EQ(Cases.Num(), 5u);
	EXPECT_EQ(FindNamedEntry(Cases, "GameToEngineSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "EngineToGameSource").GetView("ExpectedError").GetString(), "ForbiddenDependency");
	EXPECT_EQ(FindNamedEntry(Cases, "GameToPluginSource").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "ManualScanAsset").GetView("ExpectedError").GetString(), "None");
	EXPECT_EQ(FindNamedEntry(Cases, "ManualScanSource").GetView("ExpectedError").GetString(), "None");
}
