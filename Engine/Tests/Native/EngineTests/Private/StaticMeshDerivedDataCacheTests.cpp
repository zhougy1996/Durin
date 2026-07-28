#include <gtest/gtest.h>

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "EngineTestSupport.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
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
		Fixture.Root = Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		Fixture.CacheRoot = Fixture.Root / "DerivedDataCache";
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root);
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

	auto WriteU32(std::vector<Durin::uint8>& Bytes, size_t Offset, Durin::uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (Durin::uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto WriteU64(std::vector<Durin::uint8>& Bytes, size_t Offset, Durin::uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (Durin::uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto SaveVariantPackage(
		Durin::DStaticMesh& Mesh,
		const std::filesystem::path& PackagePath,
		const Durin::Asset::FCookedPayloadDescriptor& Descriptor,
		std::optional<Durin::FGuid> FirstMaterialSlotId = std::nullopt) -> void
	{
		auto* DescriptorProperty = Mesh.GetClass()->FindPropertyByName("CookedPayload");
		auto* MaterialSlotsProperty = Mesh.GetClass()->FindPropertyByName("MaterialSlots");
		ASSERT_NE(DescriptorProperty, nullptr);
		ASSERT_NE(MaterialSlotsProperty, nullptr);
		auto* StoredDescriptor = static_cast<Durin::Asset::FCookedPayloadDescriptor*>(
			DescriptorProperty->GetValuePtr(&Mesh));
		auto* MaterialSlots = static_cast<std::vector<Durin::FStaticMeshMaterialSlotDefinition>*>(
			MaterialSlotsProperty->GetValuePtr(&Mesh));
		const Durin::Asset::FCookedPayloadDescriptor SavedDescriptor = *StoredDescriptor;
		const std::vector<Durin::FStaticMeshMaterialSlotDefinition> SavedSlots = *MaterialSlots;
		*StoredDescriptor = Descriptor;
		if (FirstMaterialSlotId.has_value())
		{
			ASSERT_FALSE(MaterialSlots->empty());
			MaterialSlots->front().SlotId = *FirstMaterialSlotId;
		}
		std::vector<Durin::uint8> PackageBytes;
		const Durin::Asset::FAssetResult Result =
			Durin::Asset::SerializeAssetPackageBytes(Mesh.GetPackage(), PackageBytes);
		*StoredDescriptor = SavedDescriptor;
		*MaterialSlots = SavedSlots;
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span(PackageBytes)), PackagePath));
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

	Durin::Asset::FCookedBulkContainer DecodedBulk;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(
		FirstBulk,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		DecodedBulk,
		&Error)) << Error;
	ASSERT_EQ(DecodedBulk.Entries.size(), 1u);
	ASSERT_EQ(DecodedBulk.Payloads.size(), 1u);

	const std::filesystem::path WrongPlatformRoot =
		std::filesystem::absolute(Fixture.Root / "CookWrongPlatform");
	std::vector<Durin::uint8> WrongPlatformBulk = FirstBulk;
	WriteU32(WrongPlatformBulk, 8, static_cast<Durin::uint32>(Durin::Asset::ECookTargetPlatform::Invalid));
	std::filesystem::create_directories(WrongPlatformRoot / "Game");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstPackage)), WrongPlatformRoot / "Game/CookedMesh.dasset"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(WrongPlatformBulk)), WrongPlatformRoot / "Game/CookedMesh.dbulk"));

	const std::filesystem::path WrongSchemaRoot =
		std::filesystem::absolute(Fixture.Root / "CookWrongSchema");
	std::vector<Durin::uint8> WrongSchemaPayload = DecodedBulk.Payloads.front();
	WriteU32(WrongSchemaPayload, 4, Durin::StaticMeshPayloadSchemaVersion + 1);
	WriteU64(
		WrongSchemaPayload,
		56,
		Durin::FXxHash64::HashBuffer(std::span<const Durin::uint8>(WrongSchemaPayload).subspan(64)).HashValue);
	std::vector<Durin::uint8> WrongSchemaBulk;
	std::vector<Durin::Asset::FCookedPayloadDescriptor> WrongSchemaDescriptors;
	std::vector<Durin::Asset::FCookedBulkPayload> WrongSchemaPayloads;
	WrongSchemaPayloads.push_back({
		.PayloadId = Durin::StaticMeshPrimaryCookedPayloadId,
		.Flags = 1,
		.PayloadSchemaVersion = Durin::StaticMeshPayloadSchemaVersion,
		.Compression = Durin::Asset::ECookedPayloadCompression::None,
		.Alignment = Durin::StaticMeshPayloadAlignment,
		.Bytes = std::move(WrongSchemaPayload)});
	ASSERT_TRUE(Durin::Asset::EncodeCookedBulk(
		WrongSchemaPayloads,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		WrongSchemaBulk,
		&WrongSchemaDescriptors,
		&Error)) << Error;
	std::filesystem::create_directories(WrongSchemaRoot / "Game");
	SaveVariantPackage(
		*Fixture.Mesh,
		WrongSchemaRoot / "Game/CookedMesh.dasset",
		WrongSchemaDescriptors.front());
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(WrongSchemaBulk)), WrongSchemaRoot / "Game/CookedMesh.dbulk"));

	const std::filesystem::path CorruptPayloadRoot =
		std::filesystem::absolute(Fixture.Root / "CookCorruptPayload");
	std::vector<Durin::uint8> CorruptBulk = FirstBulk;
	ASSERT_GT(CorruptBulk.size(), 64u);
	CorruptBulk.back() ^= 0x80u;
	std::filesystem::create_directories(CorruptPayloadRoot / "Game");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstPackage)), CorruptPayloadRoot / "Game/CookedMesh.dasset"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptBulk)), CorruptPayloadRoot / "Game/CookedMesh.dbulk"));

	const std::filesystem::path MaterialMismatchRoot =
		std::filesystem::absolute(Fixture.Root / "CookMaterialMismatch");
	std::filesystem::create_directories(MaterialMismatchRoot / "Game");
	SaveVariantPackage(
		*Fixture.Mesh,
		MaterialMismatchRoot / "Game/CookedMesh.dasset",
		DecodedBulk.Entries.front(),
		Durin::FGuid::NewGuid());
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstBulk)), MaterialMismatchRoot / "Game/CookedMesh.dbulk"));

	Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
	Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "SourceAssets");
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
	ASSERT_FALSE(CookedMesh->GetRenderData()->LODResources.empty());
	ASSERT_FALSE(
		CookedMesh->GetRenderData()->LODResources.front()
			.VertexBuffers.PositionVertexBuffer.GetPositions().empty());
	ASSERT_FALSE(
		CookedMesh->GetRenderData()->LODResources.front()
			.IndexBuffer.GetIndices().empty());
	ASSERT_EQ(
		CookedMesh->GetRenderData()->MaterialSlots.size(),
		CookedMesh->GetNumMaterialSlots());
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

	auto ExpectCookedFailure = [](const std::filesystem::path& Root, std::string_view ExpectedText) {
		ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
			Durin::Asset::EPackageLoadMode::CookedRuntime, Root}));
		Durin::PathUtilities::RegisterMountPoint(
			"/Game/", (Root / "Game").generic_string() + "/");
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedMesh", Path));
		Durin::DStaticMesh* Mesh = nullptr;
		const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Mesh);
		EXPECT_FALSE(Result);
		EXPECT_EQ(Mesh, nullptr);
		EXPECT_NE(Result.Message.find(ExpectedText), std::string::npos) << Result.Message;
		Durin::Asset::ShutdownAssetManager();
	};
	ExpectCookedFailure(WrongPlatformRoot, "target");
	ExpectCookedFailure(WrongSchemaRoot, "schema version");
	ExpectCookedFailure(CorruptPayloadRoot, "checksum");
	ExpectCookedFailure(MaterialMismatchRoot, "material slot");
}
