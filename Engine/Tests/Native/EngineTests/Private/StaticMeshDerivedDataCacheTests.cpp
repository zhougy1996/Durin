#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

namespace
{
	class FScopedDerivedDataCacheRestore
	{
	public:
		FScopedDerivedDataCacheRestore()
			: PreviousDirectory(Durin::FPaths::DerivedDataCacheDir())
		{
		}

		~FScopedDerivedDataCacheRestore()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDirectory);
		}

	private:
		std::string PreviousDirectory;
	};

	struct FStaticMeshCacheFixture
	{
		std::filesystem::path Root;
		std::filesystem::path CacheRoot;
		Durin::FAssetPath AssetPath;
		Durin::DStaticMesh* Mesh = nullptr;
	};

	auto ImportCacheFixture(std::string_view Name) -> FStaticMeshCacheFixture
	{
		InitializeDObjectSystem();
		FStaticMeshCacheFixture Fixture;
		Fixture.Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / std::string(Name);
		Fixture.CacheRoot = Fixture.Root / "DerivedDataCache";
		std::filesystem::remove_all(Fixture.Root);
		std::filesystem::create_directories(Fixture.Root / "Content");
		const std::string Mount = std::format("/{}/", Name);
		Durin::PathUtilities::RegisterMountPoint(
			Mount, (Fixture.Root / "Content").generic_string() + "/");
		Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Mount + "Mesh", Fixture.AssetPath));
		const auto Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		const Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(
			Source.generic_string(), Fixture.AssetPath.ToString());
		EXPECT_TRUE(Import) << Import.Message;
		Fixture.Mesh = Import.Asset;
		if (Fixture.Mesh)
			EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
		return Fixture;
	}

	auto GetObjectPath(const FStaticMeshCacheFixture& Fixture, std::string_view Key) -> std::filesystem::path
	{
		Durin::Asset::FDerivedDataObjectStore Store(
			"StaticMesh/Objects", Durin::MaximumStaticMeshPayloadBytes);
		std::filesystem::path Path;
		std::string Error;
		EXPECT_TRUE(Store.GetObjectPath(Key, Path, &Error)) << Error;
		return Path;
	}
}

TEST(FStaticMeshDerivedDataCacheTests, ColdWarmAndSourceUnavailableLoadsFollowEditorPolicy)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCachePolicy");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string ImportedKey = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	ASSERT_EQ(ImportedKey.size(), 32u);
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, ImportedKey);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
	ASSERT_TRUE(std::filesystem::remove(ObjectPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.AssetPath, Fixture.Mesh));
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_EQ(
		Fixture.Mesh->GetDerivedDataDiagnostic().Status,
		Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Key, ImportedKey);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));

	const std::filesystem::path StoredSource =
		Fixture.Root / "SourceAssets" / "Models" / "Mesh.gltf";
	ASSERT_TRUE(std::filesystem::remove(StoredSource));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.AssetPath, Fixture.Mesh));
	EXPECT_EQ(
		Fixture.Mesh->GetDerivedDataDiagnostic().Status,
		Durin::EStaticMeshDerivedDataStatus::SourceUnavailableCached);
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	EXPECT_NE(Fixture.Mesh->GetDerivedDataDiagnostic().Message.find("Reimport"), std::string::npos);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, SourceAndSettingsChangesMissDeterministically)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheInvalidation");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string InitialKey = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	const std::filesystem::path StoredSource =
		Fixture.Root / "SourceAssets" / "Models" / "Mesh.gltf";
	{
		std::ofstream Stream(StoredSource, std::ios::binary | std::ios::app);
		ASSERT_TRUE(Stream.is_open());
		Stream << "\n";
	}
	std::string Error;
	ASSERT_TRUE(Fixture.Mesh->PostLoad(Error)) << Error;
	const std::string SourceChangedKey = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_NE(SourceChangedKey, InitialKey);
	EXPECT_NE(
		Fixture.Mesh->GetDerivedDataDiagnostic().Message.find("Rebuilt static mesh"),
		std::string::npos);
	EXPECT_NE(
		Fixture.Mesh->GetDerivedDataDiagnostic().Message.find("after cache miss"),
		std::string::npos);

	auto* SourceImportProperty = Fixture.Mesh->GetClass()->FindPropertyByName("SourceImportData");
	ASSERT_NE(SourceImportProperty, nullptr);
	auto* SourceImportData = static_cast<Durin::FStaticMeshSourceImportData*>(
		SourceImportProperty->GetValuePtr(Fixture.Mesh));
	SourceImportData->ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	ASSERT_TRUE(Fixture.Mesh->PostLoad(Error)) << Error;
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_NE(Fixture.Mesh->GetDerivedDataDiagnostic().Key, SourceChangedKey);
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, CorruptionRebuildsAndWriteFailurePreservesLiveData)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheRecovery");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string Key = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, Key);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
	const std::array<Durin::uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), ObjectPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.AssetPath, Fixture.Mesh));
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	const Durin::FStaticMeshRenderData* CompleteRenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(CompleteRenderData, nullptr);

	const std::filesystem::path BlockedCacheRoot = Fixture.Root / "BlockedCacheRoot";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), BlockedCacheRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	std::string Error;
	EXPECT_FALSE(Fixture.Mesh->PostLoad(Error));
	EXPECT_EQ(
		Fixture.Mesh->GetDerivedDataDiagnostic().Status,
		Durin::EStaticMeshDerivedDataStatus::WriteFailure);
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), CompleteRenderData);
	EXPECT_FALSE(Error.empty());

	Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, CookedPackageLoadsWithoutSourceOrDerivedDataFallback)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCookedConsumer");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::filesystem::path CookRoot = std::filesystem::absolute(Fixture.Root / "Cook");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Fixture.Root / "CookSecond");
	Durin::Asset::FCookContext First(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Fixture.Mesh->AddToCook(First, "/Game/CookedMesh", Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;

	Durin::Asset::FCookContext Second(
		SecondCookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Fixture.Mesh->AddToCook(Second, "/Game/CookedMesh", Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;
	std::vector<Durin::uint8> FirstPackage;
	std::vector<Durin::uint8> SecondPackage;
	std::vector<Durin::uint8> FirstBulk;
	std::vector<Durin::uint8> SecondBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (CookRoot / "Game/CookedMesh.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondCookRoot / "Game/CookedMesh.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstBulk, (CookRoot / "Game/CookedMesh.dbulk").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondBulk, (SecondCookRoot / "Game/CookedMesh.dbulk").generic_string()));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstBulk, SecondBulk);
	auto ContainsText = [](std::span<const Durin::uint8> Bytes, std::string_view Text) {
		return std::search(
			Bytes.begin(), Bytes.end(),
			reinterpret_cast<const Durin::uint8*>(Text.data()),
			reinterpret_cast<const Durin::uint8*>(Text.data() + Text.size())) != Bytes.end();
	};
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceFile"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceImportData"));
	EXPECT_FALSE(ContainsText(FirstPackage, "Assimp"));

	std::filesystem::remove_all(Fixture.CacheRoot);
	std::filesystem::remove_all(Fixture.Root / "SourceAssets");
	Durin::Asset::ShutdownAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, CookRoot}));
	Durin::PathUtilities::RegisterMountPoint(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	Durin::FAssetPath CookedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedMesh", CookedPath));
	Durin::DStaticMesh* CookedMesh = nullptr;
	const Durin::Asset::FAssetResult LoadResult = Durin::Asset::LoadAsset(CookedPath, CookedMesh);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(CookedMesh, nullptr);
	ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
	EXPECT_EQ(
		CookedMesh->GetDerivedDataDiagnostic().Status,
		Durin::EStaticMeshDerivedDataStatus::CookedLoaded);
	EXPECT_FALSE(CookedMesh->GetSourceImportData().HasSource());
	EXPECT_EQ(
		CookedMesh->GetCookedPayloadDescriptor().PayloadId,
		Durin::StaticMeshPrimaryCookedPayloadId);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(CookedPath));
	ASSERT_TRUE(std::filesystem::remove(CookRoot / "Game/CookedMesh.dbulk"));
	CookedMesh = nullptr;
	const Durin::Asset::FAssetResult MissingBulk = Durin::Asset::LoadAsset(CookedPath, CookedMesh);
	EXPECT_FALSE(MissingBulk);
	EXPECT_EQ(CookedMesh, nullptr);
	EXPECT_NE(MissingBulk.Message.find("Cooked static mesh"), std::string::npos);
	Durin::Asset::ShutdownAssetManager();
}
