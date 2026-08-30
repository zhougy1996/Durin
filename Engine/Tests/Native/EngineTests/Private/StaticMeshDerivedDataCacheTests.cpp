#include <gtest/gtest.h>

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "EngineTestSupport.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

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
		std::filesystem::path SourcePath;
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
		Durin::Testing::RegisterMountPointForTests(
			Mount, (Fixture.Root / "Content").generic_string() + "/");
		Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Mount + "Mesh", Fixture.AssetPath));
		const auto Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		Fixture.SourcePath = Fixture.Root / "Sources/Mesh.gltf";
		std::filesystem::create_directories(Fixture.SourcePath.parent_path());
		std::filesystem::copy_file(Source, Fixture.SourcePath,
			std::filesystem::copy_options::overwrite_existing);
		const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
			Fixture.SourcePath.generic_string(), Fixture.AssetPath.ToString());
		EXPECT_TRUE(Import) << Import.Message;
		Fixture.Mesh = Import.Asset;
		if (Fixture.Mesh)
			EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
		return Fixture;
	}

	auto GetObjectPath(const FStaticMeshCacheFixture& Fixture, std::string_view Key) -> std::filesystem::path
	{
		return Fixture.CacheRoot / "StaticMesh" / "Objects"
			/ std::string(Key.substr(0, 2)) / (std::string(Key) + ".bin");
	}

	auto WriteU32(std::vector<std::byte>& Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(std::vector<std::byte>& Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto ReadU64(const std::vector<std::byte>& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto RefreshEnvelopeHeaderHash(std::vector<std::byte>& Bytes) -> void
	{
		const uint64 HeaderBytes = ReadU64(Bytes, 32);
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)));
		WriteU64(Bytes, 48, Hash.HashLow);
		WriteU64(Bytes, 56, Hash.HashHigh);
	}

	auto RestartAssetManager(const std::filesystem::path& CookRoot = {}) -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		if (CookRoot.empty())
		{
			ASSERT_TRUE(Durin::Asset::InitializeAssetManager());
			return;
		}
		auto Configuration = Durin::Asset::FAssetRuntimeConfiguration::Authored();
		ASSERT_TRUE(Durin::Asset::FAssetRuntimeConfiguration::Cooked(
			CookRoot, Configuration));
		ASSERT_TRUE(Durin::Asset::InitializeAssetManager(std::move(Configuration)));
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
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Key, ImportedKey);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		Fixture.AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));

	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.AssetPath, Fixture.Mesh));
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().Status
		== Durin::EStaticMeshDerivedDataStatus::Hit
		|| Fixture.Mesh->GetDerivedDataDiagnostic().Status
		== Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, SourceAndSettingsChangesMissDeterministically)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheInvalidation");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string InitialKey = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	{
		std::ofstream Stream(Fixture.SourcePath, std::ios::binary | std::ios::app);
		ASSERT_TRUE(Stream.is_open());
		Stream << "\n";
	}
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	const std::string SourceChangedKey = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().Status
		== Durin::EStaticMeshDerivedDataStatus::Hit
		|| Fixture.Mesh->GetDerivedDataDiagnostic().Status
		== Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_EQ(SourceChangedKey, InitialKey);
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().Message.empty());

	auto* ImportData = dynamic_cast<Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Fixture.Mesh->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto State = ImportData->GetStaticMeshState();
	State.ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	ASSERT_TRUE(ImportData->SetState(std::move(State), Error)) << Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_NE(Fixture.Mesh->GetDerivedDataDiagnostic().Key, SourceChangedKey);
	EXPECT_TRUE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		Fixture.AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, CorruptionRecoveryIsNonPersistentAndFailurePreservesLiveData)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheRecovery");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string Key = Fixture.Mesh->GetDerivedDataDiagnostic().Key;
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, Key);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Fixture.AssetPath));
	const std::array<uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), ObjectPath));
	const std::filesystem::path PackagePath = Fixture.Root / "Content" / "Mesh.dasset";
	std::vector<std::byte> PackageBytesBeforeRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesBeforeRecovery, PackagePath));
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(PackagePath, PackageTimeBeforeRecovery);

	ASSERT_TRUE(Durin::Asset::LoadAsset(Fixture.AssetPath, Fixture.Mesh));
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status, Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	const Durin::FStaticMeshRenderData* CompleteRenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(CompleteRenderData, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	std::vector<std::byte> PackageBytesAfterRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesAfterRecovery, PackagePath));
	EXPECT_EQ(PackageBytesAfterRecovery, PackageBytesBeforeRecovery);
	EXPECT_EQ(std::filesystem::last_write_time(PackagePath), PackageTimeBeforeRecovery);

	const std::filesystem::path BlockedCacheRoot = Fixture.Root / "BlockedCacheRoot";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), BlockedCacheRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	std::string Error;
	EXPECT_TRUE(Fixture.Mesh->PostLoad(Error)) << Error;
	EXPECT_TRUE(Error.empty());
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Fixture.Mesh->GetDerivedDataDiagnostic().Status,
		Durin::EStaticMeshDerivedDataStatus::Rebuilt);
	EXPECT_FALSE(Fixture.Mesh->GetDerivedDataDiagnostic().Message.empty());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());

	Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		Fixture.AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, CookedCollisionCompanionIsDeterministicAndRequiredAtRuntime)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCookedCollisionConsumer");
	ASSERT_NE(Fixture.Mesh, nullptr);
	std::string Error;
	ASSERT_TRUE(Fixture.Mesh->SetCollisionSourceMode(
		Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	ASSERT_NE(Fixture.Mesh->GetBodySetup(), nullptr);
	Durin::FCollisionGeometryRef AuthoredGeometry;
	ASSERT_TRUE(Fixture.Mesh->GetBodySetup()->BuildComplexGeometry(AuthoredGeometry));
	const uint32 AuthoredVertices = AuthoredGeometry.GetVertexCount();
	const uint32 AuthoredTriangles = AuthoredGeometry.GetTriangleCount();
	const uint32 AuthoredNodes = AuthoredGeometry.GetNodeCount();
	const uint64 AuthoredBytes = AuthoredGeometry.GetRetainedBytes();

	const std::filesystem::path CookRoot = std::filesystem::absolute(Fixture.Root / "CookCollision");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Fixture.Root / "CookCollisionSecond");
	for (const std::filesystem::path& Root : {CookRoot, SecondCookRoot})
	{
		Durin::Asset::FCookContext Context(
			Root, Durin::Asset::ECookTargetPlatform::Win64,
			Durin::Asset::ECookTargetProfile::Game);
		ASSERT_TRUE(Durin::Asset::ContributeEngineCookAsset(
			*Fixture.Mesh, "/Game/CookedCollisionMesh", Context, Error)) << Error;
		ASSERT_TRUE(Context.Publish(&Error)) << Error;
	}
	std::vector<std::byte> FirstPackage, SecondPackage, FirstBulk, SecondBulk, FirstManifest, SecondManifest;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstPackage, (CookRoot / "Game/CookedCollisionMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondPackage, (SecondCookRoot / "Game/CookedCollisionMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstManifest, (CookRoot / "CookManifest.bin")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondManifest, (SecondCookRoot / "CookManifest.bin")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstManifest, SecondManifest);
	if (!Durin::FFileHelper::LoadFileToArray(
		FirstBulk, CookRoot / "Game/CookedCollisionMesh.dbulk"))
	{
		EXPECT_FALSE(std::filesystem::exists(
			SecondCookRoot / "Game/CookedCollisionMesh.dbulk"));
		Durin::Asset::FAssetPackageInspection Inspection;
		ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
			(CookRoot / "Game/CookedCollisionMesh.dasset").generic_string(), Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		EXPECT_NE(Inspection.FindField("CollisionData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		RestartAssetManager(CookRoot);
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedCollisionMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		const Durin::Asset::FAssetResult Loaded =
			Durin::Asset::LoadAsset(Path, CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		const Durin::FCookedMeshBlockingResult LoadResult =
			CookedMesh->EnsureRenderDataAndResourcesBlocking();
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		Durin::FCollisionGeometryRef Geometry;
		ASSERT_TRUE(CookedMesh->GetBodySetup()->BuildComplexGeometry(Geometry));
		EXPECT_EQ(Geometry.GetVertexCount(), AuthoredVertices);
		EXPECT_EQ(Geometry.GetTriangleCount(), AuthoredTriangles);
		EXPECT_EQ(Geometry.GetNodeCount(), AuthoredNodes);
		EXPECT_EQ(Geometry.GetRetainedBytes(), AuthoredBytes);
		ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
		RestartAssetManager();
		return;
	}
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
	ASSERT_TRUE(Durin::Asset::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", First, Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;

	Durin::Asset::FCookContext Second(
		SecondCookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::Asset::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", Second, Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;
	std::vector<std::byte> FirstPackage;
	std::vector<std::byte> SecondPackage;
	std::vector<std::byte> FirstBulk;
	std::vector<std::byte> SecondBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (CookRoot / "Game/CookedMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondCookRoot / "Game/CookedMesh.dasset")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	if (!Durin::FFileHelper::LoadFileToArray(
		FirstBulk, CookRoot / "Game/CookedMesh.dbulk"))
	{
		EXPECT_FALSE(std::filesystem::exists(
			SecondCookRoot / "Game/CookedMesh.dbulk"));
		Durin::Asset::FAssetPackageInspection Inspection;
		ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
			(CookRoot / "Game/CookedMesh.dasset").generic_string(), Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		RestartAssetManager(CookRoot);
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		const Durin::Asset::FAssetResult Loaded =
			Durin::Asset::LoadAsset(Path, CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		auto* FirstConsumer = Durin::NewObject<Durin::DStaticMeshComponent>(
			nullptr, Durin::FName("CookedStaticMeshFirstConsumer"));
		FirstConsumer->SetStaticMesh(CookedMesh);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		ASSERT_TRUE(Durin::Asset::InitializeCookedMeshLoadManager());
		FirstConsumer->RegisterComponent();
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::IoQueued);
		EXPECT_EQ(FirstConsumer->CreateSceneProxy(), nullptr);
		Durin::Asset::ShutdownCookedMeshLoadManager();
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Cancelled);
		EXPECT_NE(CookedMesh->GetDerivedDataDiagnostic().Message.find("cancel"),
			std::string::npos);
		ASSERT_TRUE(Durin::Asset::InitializeCookedMeshLoadManager());
		const Durin::FCookedMeshBlockingResult RetryResult =
			CookedMesh->RetryRenderDataAndResourcesBlocking();
		ASSERT_TRUE(RetryResult) << RetryResult.Message;
		auto FirstProxy = FirstConsumer->CreateSceneProxy();
		const Durin::FCookedMeshLoadStatus RecoveredStatus =
			CookedMesh->RequestRenderDataAndResources();
		const Durin::Asset::FCookedMeshLoadDiagnostics RecoveredDiagnostics =
			Durin::Asset::GetCookedMeshLoadManager()->GetDiagnostics();
		if (!FirstProxy) Durin::Asset::ShutdownCookedMeshLoadManager();
		ASSERT_NE(FirstProxy, nullptr)
			<< "cpu_phase=" << static_cast<uint32>(RecoveredStatus.CpuPhase)
			<< " failed=" << RecoveredDiagnostics.FailedCount
			<< " stale=" << RecoveredDiagnostics.StaleCount
			<< " in_flight=" << RecoveredDiagnostics.InFlightCount
			<< " diagnostic=" << CookedMesh->GetDerivedDataDiagnostic().Message;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::CpuReady);
		auto* SplineConsumer = Durin::NewObject<Durin::DSplineMeshComponent>(
			nullptr, Durin::FName("CookedSplineMeshConsumer"));
		SplineConsumer->SetStaticMesh(CookedMesh);
		ASSERT_NE(SplineConsumer->CreateSceneProxy(), nullptr);
		Durin::Asset::ShutdownCookedMeshLoadManager();
		EXPECT_EQ(CookedMesh->GetAssetImportData(), nullptr);
		EXPECT_NE(CookedMesh->GetCookedRenderData().GetMetadata().LogicalSize, 0u);
		ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
		RestartAssetManager();
		return;
	}
}
