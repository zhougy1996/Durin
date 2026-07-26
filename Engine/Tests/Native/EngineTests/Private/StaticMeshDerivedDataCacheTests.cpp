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
