#include <gtest/gtest.h>
#include "NativeAssetRuntimeTestSupport.h"

#include "NativeDObjectTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Property.h"
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
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

namespace
{
	class FScopedStaticMeshProviderRestore
	{
	public:
		~FScopedStaticMeshProviderRestore()
		{
			Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
		}
	};

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
		Durin::FPackagePath AssetPath;
		Durin::DStaticMesh* Mesh = nullptr;
	};

	auto GetStaticMeshKey(const Durin::DStaticMesh& Mesh) -> std::string
	{
		std::string Error;
		const Durin::FCacheKeyProxy Key = Durin::BuildStaticMeshDerivedDataKey({
			.ImportedDataHash = Mesh.GetImportedData().GetIdentity(),
			.ReconciliationHash = Durin::BuildStaticMeshReconciliationHash(
				Mesh.GetMaterialSlots(), Mesh.GetNormalizedSize()),
			.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64}, Error);
		EXPECT_TRUE(Key.IsValid()) << Error;
		return Key.ToString();
	}

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
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Mount + "Mesh", Fixture.AssetPath));
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
			EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
		return Fixture;
	}

	auto GetObjectPath(const FStaticMeshCacheFixture& Fixture, std::string_view Key) -> std::filesystem::path
	{
		return Fixture.CacheRoot / "StaticMesh" / "Objects"
			/ std::string(Key.substr(0, 2)) / (std::string(Key) + ".bin");
	}

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(Durin::FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto ReadU64(const Durin::FByteBuffer& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto RefreshEnvelopeHeaderHash(Durin::FByteBuffer& Bytes) -> void
	{
		const uint64 HeaderBytes = ReadU64(Bytes, 32);
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)));
		WriteU64(Bytes, 48, Hash.HashLow);
		WriteU64(Bytes, 56, Hash.HashHigh);
	}

}

TEST(FStaticMeshDerivedDataCacheTests, EngineProviderPathPreservesKeysAndRecoversCorruption)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshEngineProvider");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string BaselineKey = GetStaticMeshKey(*Fixture.Mesh);
	FStaticMeshBuildRequest Request{
		.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()};
	FStaticMeshBuildResult Product;
	std::string Error;
	// The runtime loader carries only authored metadata until a cache miss.
	Request.ImportedData.ReleaseGeometry();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_TRUE(Product.DiagnosticMessage.empty());
	EXPECT_TRUE(Request.ImportedData.IsValid());
	ASSERT_NE(Product.RenderData, nullptr);
	const std::array<std::byte, 4> Corrupt{};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, GetObjectPath(Fixture, BaselineKey)));
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_TRUE(Request.ImportedData.IsValid());
	EXPECT_FALSE(Product.DiagnosticMessage.empty());
	EXPECT_TRUE(Error.empty());
	Request.Reconciliation.MaterialSlots.clear();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.MaterialSlots.size(), Fixture.Mesh->GetMaterialSlots().size());

	FStaticMeshCollisionBuildResult ColdCollision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(
		*Fixture.Mesh->GetRenderData(), EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, ColdCollision, Error)) << Error;
	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error)) << Error;
	EXPECT_EQ(Collision.DerivedDataKey, ColdCollision.DerivedDataKey);
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Collision.PayloadBytes, ColdCollision.PayloadBytes);
	const auto CollisionPath = Fixture.CacheRoot / "StaticMeshCollision/Objects"
		/ Collision.DerivedDataKey.ToString().substr(0, 2)
		/ (Collision.DerivedDataKey.ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(CollisionPath));
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, false)) << Error;
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_FALSE(std::filesystem::exists(CollisionPath));
	const auto BlockedRoot = Fixture.Root / "BlockedCollisionCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, BlockedRoot));
	FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	// A read failure remains observable even when no Put is attempted.
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, false)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_FALSE(Collision.Diagnostic.empty());
	EXPECT_EQ(Collision.CacheWriteNanoseconds, 0u);
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_FALSE(Collision.Diagnostic.empty());
	EXPECT_LE(Collision.Diagnostic.size(), 2048u);
	EXPECT_NE(Collision.Diagnostic.find("Read: "), std::string::npos);
	EXPECT_NE(Collision.Diagnostic.find("; Put: "), std::string::npos);
	FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, InvalidDetachedReplacementPreservesLiveState)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshReplacementRollback");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const FStaticMeshRenderData* Original = Fixture.Mesh->GetRenderData();
	const FXxHash128 ImportedIdentity = Fixture.Mesh->GetImportedData().GetIdentity();
	const uint64 Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	FStaticMeshBuildResult Result;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData({
		.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()}, Result, Error)) << Error;
	ASSERT_FALSE(Result.RenderData->LODResources.empty());
	Result.RenderData->LODResources.front().IndexBuffer.GetMutableIndices().front() =
		std::numeric_limits<uint32>::max();
	EXPECT_FALSE(ApplyStaticMeshBuildResult(*Fixture.Mesh, Fixture.Mesh->GetImportedData(), std::move(Result), Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), Original);
	EXPECT_EQ(Fixture.Mesh->GetImportedData().GetIdentity(), ImportedIdentity);
	EXPECT_EQ(Fixture.Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, ColdWarmAndSourceUnavailableLoadsFollowEditorPolicy)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCachePolicy");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string ImportedKey = GetStaticMeshKey(*Fixture.Mesh);
	ASSERT_EQ(ImportedKey.size(), 32u);
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, ImportedKey);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
	ASSERT_TRUE(std::filesystem::remove(ObjectPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(GetStaticMeshKey(*Fixture.Mesh), ImportedKey);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));

	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, SourceAndSettingsChangesMissDeterministically)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheInvalidation");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string InitialKey = GetStaticMeshKey(*Fixture.Mesh);
	{
		std::ofstream Stream(Fixture.SourcePath, std::ios::binary | std::ios::app);
		ASSERT_TRUE(Stream.is_open());
		Stream << "\n";
	}
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	const std::string SourceChangedKey = GetStaticMeshKey(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(SourceChangedKey, InitialKey);

	auto* ImportData = dynamic_cast<Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Fixture.Mesh->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto State = ImportData->GetStaticMeshState();
	State.ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	ASSERT_TRUE(ImportData->SetState(std::move(State), Error)) << Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_NE(GetStaticMeshKey(*Fixture.Mesh), SourceChangedKey);
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, CorruptionRecoveryIsNonPersistentAndFailurePreservesLiveData)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheRecovery");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string Key = GetStaticMeshKey(*Fixture.Mesh);
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, Key);
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
	const std::array<uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), ObjectPath));
	const std::filesystem::path PackagePath = Fixture.Root / "Content" / "Mesh.dasset";
	Durin::FByteBuffer PackageBytesBeforeRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesBeforeRecovery, PackagePath));
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(PackagePath, PackageTimeBeforeRecovery);

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	const Durin::FStaticMeshRenderData* CompleteRenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(CompleteRenderData, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	Durin::FByteBuffer PackageBytesAfterRecovery;
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
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());

	Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
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

	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	const std::filesystem::path CookRoot = std::filesystem::absolute(Fixture.Root / "CookCollision");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Fixture.Root / "CookCollisionSecond");
	for (const std::filesystem::path& Root : {CookRoot, SecondCookRoot})
	{
		Durin::FCookContext Context(
			Root, Durin::ECookTargetPlatform::Win64,
			Durin::ECookTargetProfile::Game);
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Fixture.Mesh, "/Game/CookedCollisionMesh", Context, Error)) << Error;
		ASSERT_TRUE(Context.Publish(&Error)) << Error;
	}
	Durin::FByteBuffer FirstPackage, SecondPackage, FirstBulk, SecondBulk, FirstManifest, SecondManifest;
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
		Durin::FAssetPackageInspection Inspection;
		Durin::FPackagePath CookedPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
			"/Game/CookedCollisionMesh", CookedPath));
		ASSERT_TRUE(Durin::InspectAssetPackage(
			(CookRoot / "Game/CookedCollisionMesh.dasset").generic_string(),
			CookedPath, Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		EXPECT_NE(Inspection.FindField("CollisionData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		const FScopedStaticMeshProviderRestore ProviderRestore;
		ASSERT_TRUE(Durin::FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedCollisionMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		ASSERT_FALSE(Durin::FModuleManager::Get().IsModuleLoaded("StaticMeshBuild"));
		const Durin::FAssetResult Loaded =
			Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()), CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		const Durin::FCookedMeshBlockingResult LoadResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		Durin::FCollisionGeometryRef Geometry;
		ASSERT_TRUE(CookedMesh->GetBodySetup()->BuildComplexGeometry(Geometry));
		EXPECT_EQ(Geometry.GetVertexCount(), AuthoredVertices);
		EXPECT_EQ(Geometry.GetTriangleCount(), AuthoredTriangles);
		EXPECT_EQ(Geometry.GetNodeCount(), AuthoredNodes);
		EXPECT_EQ(Geometry.GetRetainedBytes(), AuthoredBytes);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
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
	Durin::FCookContext First(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", First, Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;

	Durin::FCookContext Second(
		SecondCookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", Second, Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;
	Durin::FByteBuffer FirstPackage;
	Durin::FByteBuffer SecondPackage;
	Durin::FByteBuffer FirstBulk;
	Durin::FByteBuffer SecondBulk;
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
		Durin::FAssetPackageInspection Inspection;
		Durin::FPackagePath CookedPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
			"/Game/CookedMesh", CookedPath));
		ASSERT_TRUE(Durin::InspectAssetPackage(
			(CookRoot / "Game/CookedMesh.dasset").generic_string(),
			CookedPath, Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		const FScopedStaticMeshProviderRestore ProviderRestore;
		ASSERT_TRUE(Durin::FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		ASSERT_FALSE(Durin::FModuleManager::Get().IsModuleLoaded("StaticMeshBuild"));
		const Durin::FAssetResult Loaded =
			Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()), CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		auto* FirstConsumer = Durin::NewObject<Durin::DStaticMeshComponent>(
			nullptr, Durin::FName("CookedStaticMeshFirstConsumer"));
		FirstConsumer->SetStaticMesh(CookedMesh);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
		FirstConsumer->RegisterComponent();
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::IoQueued);
		EXPECT_EQ(FirstConsumer->CreateSceneProxy(), nullptr);
		Durin::ShutdownCookedMeshLoadManager();
		const auto CancelledStatus = CookedMesh->RequestRenderDataAndResources();
		EXPECT_EQ(CancelledStatus.CpuPhase, Durin::ECookedMeshCpuPhase::Cancelled);
		ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
		const Durin::FCookedMeshBlockingResult RetryResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(RetryResult) << RetryResult.Message;
		EXPECT_GT(RetryResult.Status.Generation, CancelledStatus.Generation);
		auto FirstProxy = FirstConsumer->CreateSceneProxy();
		const Durin::FCookedMeshLoadStatus RecoveredStatus =
			CookedMesh->RequestRenderDataAndResources();
		const Durin::FCookedMeshLoadDiagnostics RecoveredDiagnostics =
			Durin::GetCookedMeshLoadManager()->GetDiagnostics();
		if (!FirstProxy) Durin::ShutdownCookedMeshLoadManager();
		ASSERT_NE(FirstProxy, nullptr)
			<< "cpu_phase=" << static_cast<uint32>(RecoveredStatus.CpuPhase)
			<< " failed=" << RecoveredDiagnostics.FailedCount
			<< " stale=" << RecoveredDiagnostics.StaleCount
			<< " in_flight=" << RecoveredDiagnostics.InFlightCount;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::CpuReady);
		auto* SplineConsumer = Durin::NewObject<Durin::DSplineMeshComponent>(
			nullptr, Durin::FName("CookedSplineMeshConsumer"));
		SplineConsumer->SetStaticMesh(CookedMesh);
		ASSERT_NE(SplineConsumer->CreateSceneProxy(), nullptr);
		Durin::ShutdownCookedMeshLoadManager();
		EXPECT_EQ(CookedMesh->GetAssetImportData(), nullptr);
		EXPECT_FALSE(CookedMesh->GetImportedData().IsValid());
		EXPECT_FALSE(CookedMesh->GetImportedData().IsGeometryResident());
		EXPECT_NE(CookedMesh->GetCookedRenderData().GetMetadata().LogicalSize, 0u);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
		return;
	}
}

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif
TEST(FStaticMeshSourceResidencyTests, RepresentativeGeometry)
{
	using namespace Durin;
	for (const uint32 Triangles : {1u, 100000u})
	{
		#if defined(__APPLE__)
		std::atomic<size_t> PeakBytes{0};
		std::jthread Sampler([&](std::stop_token Stop) {
			while (!Stop.stop_requested())
			{
				malloc_statistics_t Stats{};
				malloc_zone_statistics(malloc_default_zone(), &Stats);
				PeakBytes.store(std::max(PeakBytes.load(), Stats.size_in_use));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
#endif
		FStaticMeshDecodedGeometry Input;
		Input.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Mesh = Input.Meshes.emplace_back();
		Mesh.Name = "Fixture";
		for (uint32 Triangle = 0; Triangle < Triangles; ++Triangle)
		{
			const float X = static_cast<float>(Triangle);
			Mesh.Positions.insert(Mesh.Positions.end(), {{X, 0, 0}, {X + 1, 0, 0}, {X, 1, 0}});
			Mesh.Indices.insert(Mesh.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		std::string Error;
		FStaticMeshImportedData Source;
		ASSERT_TRUE(Source.Initialize(std::move(Input), Error)) << Error;
		auto Decoded = Source.AcquireGeometry(Error);
		ASSERT_TRUE(Error.empty()) << Error;
		EXPECT_EQ(Decoded->Meshes.front().Positions.size(), Triangles * 3);
		EXPECT_EQ(Source.AcquireGeometry(Error), Decoded);
		EXPECT_EQ(Source.GetGeometryBulk().GetPayloadSize(), Triangles == 1 ? 195u : 4800147u);
		EXPECT_EQ(Source.GetIdentity().HashLow, Triangles == 1 ? 4982799754724307949ull : 17565407108445809865ull);
		EXPECT_EQ(Source.GetIdentity().HashHigh, Triangles == 1 ? 10298414200299834774ull : 892654471079648671ull);
		FStaticMeshBuildResult Product;
		ASSERT_TRUE(BuildStaticMeshDerivedData({.ImportedData = Source, .bPersistDerivedData = false}, Product, Error)) << Error;
		const uint64 Retained = Mesh.Positions.capacity() * sizeof(FVector3f)
			+ Mesh.Indices.capacity() * sizeof(uint32);
		std::cout << "residency_fixture triangles=" << Triangles << " retained_array_capacity_bytes=" << Retained
			<< " payload_bytes=" << Source.GetGeometryBulk().GetPayloadSize()
			<< " identity=" << Source.GetIdentity().HashLow << ":" << Source.GetIdentity().HashHigh;
#if defined(__APPLE__)
		Sampler.request_stop();
		Sampler.join();
		std::cout << " process_sampled_peak_allocated_bytes=" << PeakBytes.load();
#endif
		std::cout << std::endl;
	}
}

namespace
{
	auto MakeResidencyGeometry() -> Durin::FStaticMeshDecodedGeometry
	{
		Durin::FStaticMeshDecodedGeometry Geometry;
		Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Mesh = Geometry.Meshes.emplace_back();
		Mesh.Name = "Fixture";
		Mesh.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
		Mesh.Indices = {0, 1, 2};
		return Geometry;
	}

	// Exercises the persisted field boundary without exposing mutable bulk in the source API.
	auto GetReflectedSourceBulk(Durin::FStaticMeshImportedData& Source) -> Durin::FEditorBulkData&
	{
		auto* Property = Durin::FStaticMeshImportedData::StaticStruct()->FindPropertyByName("Geometry");
		return *Property->ContainerPtrToValuePtr<Durin::FEditorBulkData>(&Source);
	}

	class FResidencyReadProbe final : public Durin::FPackageResource
	{
	public:
		explicit FResidencyReadProbe(Durin::FSharedByteBuffer InBytes, bool InFail = false)
			: FPackageResource(InBytes.GetSize()), Bytes(std::move(InBytes)), bFail(InFail) {}

	private:
		auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool&)
			-> Durin::FPackageResourceReadResult override
		{
			if (bFail) return {.Status = Durin::EPackageResourceReadStatus::IoError,
				.Message = "Deliberate residency source read failure."};
			return {.Status = Durin::EPackageResourceReadStatus::Success,
				.Buffer = Bytes.MakeView(Offset, Size)};
		}
		Durin::FSharedByteBuffer Bytes;
		bool bFail;
	};

	auto AttachResidencyProbe(Durin::FStaticMeshImportedData& Source, bool bFail = false)
		-> std::shared_ptr<FResidencyReadProbe>
	{
		const auto& Bulk = Source.GetGeometryBulk();
		auto Resource = std::make_shared<FResidencyReadProbe>(Bulk.GetPayload().Wait().Buffer, bFail);
		Durin::FEditorBulkData Attached;
		std::string Error;
		EXPECT_TRUE(Durin::FEditorBulkData::TryCreatePackageBacked(Bulk.GetInstanceId(),
			Bulk.GetPayloadId(), Bulk.GetPayloadSize(),
			{.Resource = Resource, .StoredSize = Bulk.GetPayloadSize()}, Attached, &Error)) << Error;
		Source.ReleaseGeometry();
		GetReflectedSourceBulk(Source) = std::move(Attached);
		return Resource;
	}
}

TEST(FStaticMeshSourceResidencyTests, SharesConcurrentReadsAndSurvivesReleaseCopyAndReplacement)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	ASSERT_TRUE(Source.IsGeometryResident());
	const auto Resource = AttachResidencyProbe(Source);
	const auto Identity = Source.GetIdentity();
	EXPECT_TRUE(Source.IsValid());
	EXPECT_EQ(Source.GetMeshCount(), 1u);
	EXPECT_EQ(Source.GetMaterialSlotCount(), 1u);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	std::array<FStaticMeshGeometryReadHandle, 16> Handles;
	std::vector<std::jthread> Readers;
	for (auto& Handle : Handles)
		Readers.emplace_back([&Source, &Handle] {
			std::string ReadError;
			Handle = Source.AcquireGeometry(ReadError);
			EXPECT_TRUE(ReadError.empty()) << ReadError;
		});
	Readers.clear();
	ASSERT_TRUE(Handles.front());
	for (const auto& Handle : Handles) EXPECT_EQ(Handle, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	FStaticMeshImportedData Copy = Source;
	Source.ReleaseGeometry();
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Copy.AcquireGeometry(Error), Handles.front());
	const auto Reload = Source.AcquireGeometry(Error);
	ASSERT_TRUE(Reload) << Error;
	EXPECT_NE(Reload, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	std::weak_ptr<const FStaticMeshDecodedGeometry> Weak = Handles.front();
	Handles.fill({});
	EXPECT_FALSE(Weak.expired());
	Copy.ReleaseGeometry();
	EXPECT_TRUE(Weak.expired());
	const auto Old = Source.AcquireGeometry(Error);
	auto Replacement = MakeResidencyGeometry();
	Replacement.Meshes.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Replacement), Error)) << Error;
	EXPECT_NE(Source.GetIdentity(), Identity);
	EXPECT_EQ(Old->Meshes.front().Positions.front().x, 0);
	EXPECT_EQ(Source.AcquireGeometry(Error)->Meshes.front().Positions.front().x, 7);
	std::jthread Releaser([&] { for (int Index = 0; Index < 32; ++Index) Source.ReleaseGeometry(); });
	for (int Index = 0; Index < 32; ++Index)
	{
		const auto Handle = Source.AcquireGeometry(Error);
		ASSERT_TRUE(Handle) << Error;
		EXPECT_EQ(Handle->Meshes.front().Positions.front().x, 7);
	}
}

TEST(FStaticMeshSourceResidencyTests, InvalidCompleteInitializationPreservesIdentityAndReaders)
{
	using namespace Durin;
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	const auto Original = Source.AcquireGeometry(Error);
	const auto Identity = Source.GetIdentity();
	for (uint32 Case = 0; Case < 7; ++Case)
	{
		auto Invalid = MakeResidencyGeometry();
		auto& Mesh = Invalid.Meshes.front();
		switch (Case)
		{
		case 0: Mesh.Indices.front() = 3; break;
		case 1: Mesh.Normals.resize(1); break;
		case 2: Mesh.UVChannels[3].resize(2); break;
		case 3: Mesh.Colors.resize(1); break;
		case 4: Mesh.Tangents.resize(1); break;
		case 5: Mesh.Name.resize(4097); break;
		case 6: Invalid.MaterialSlots.clear(); break;
		}
		EXPECT_FALSE(Source.Initialize(std::move(Invalid), Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_EQ(Source.GetIdentity(), Identity);
		EXPECT_EQ(Source.AcquireGeometry(Error), Original);
	}
}

TEST(FStaticMeshSourceResidencyTests, MalformedCanonicalBytesNeverPublishPartialResidency)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	const auto Payload = Source.GetGeometryBulk().GetPayload().Wait();
	const FByteBuffer Original(Payload.Buffer.begin(), Payload.Buffer.end());
	ASSERT_EQ(Original.size(), 195u);
	for (uint32 Case = 0; Case < 6; ++Case)
	{
		FByteBuffer Bytes = Original;
		switch (Case)
		{
		case 0: Bytes.pop_back(); break;
		case 1: Bytes.push_back(std::byte{}); break;
		case 2: WriteU64(Bytes, 75, 50'000'000); break;
		case 3: WriteU32(Bytes, Bytes.size() - 4, 3); break;
		case 5: WriteU64(Bytes, 75, std::numeric_limits<uint64>::max()); break;
		case 4:
			WriteU64(Bytes, 119, 1);
			Bytes.insert(Bytes.begin() + 127, 12, std::byte{});
			break;
		}
		ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Bytes));
		Error = "stale diagnostic";
		EXPECT_FALSE(Source.AcquireGeometry(Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_NE(Error, "stale diagnostic");
		EXPECT_FALSE(Source.IsGeometryResident());
	}
	ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Original));
	ASSERT_TRUE(Source.AcquireGeometry(Error)) << Error;
}

TEST(FStaticMeshSourceResidencyTests, WarmCacheSkipsUnreadableBulkAndMissPreservesReadDiagnostic)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshUnreadableResidency");
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetImportedData().IsGeometryResident());
	FStaticMeshBuildRequest Request{.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()};
	const auto Resource = AttachResidencyProbe(Request.ImportedData, true);
	FStaticMeshBuildResult Product;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, GetStaticMeshKey(*Fixture.Mesh))));
	EXPECT_FALSE(BuildStaticMeshDerivedData(Request, Product, Error));
	EXPECT_EQ(Error, "Deliberate residency source read failure.");
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	EXPECT_FALSE(Request.ImportedData.IsGeometryResident());
	EXPECT_FALSE(BuildStaticMeshDerivedData(Request, Product, Error));
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshSourceResidencyTests, ExistingAuthoredPackageAndDuplicateRetainCanonicalSource)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Models/Box", Path));
	DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Mesh));
	ASSERT_NE(Mesh, nullptr);
	std::string Error;
	const auto Geometry = Mesh->GetImportedData().AcquireGeometry(Error);
	ASSERT_TRUE(Geometry) << Error;
	auto* Duplicate = Cast<DStaticMesh>(DuplicateObject(Mesh, nullptr, "ResidencyDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetImportedData().GetIdentity(), Mesh->GetImportedData().GetIdentity());
	Mesh->GetImportedData().ReleaseGeometry();
	const auto Copied = Duplicate->GetImportedData().AcquireGeometry(Error);
	ASSERT_TRUE(Copied) << Error;
	EXPECT_EQ(Copied->Meshes.size(), Geometry->Meshes.size());
	EXPECT_EQ(Copied->Meshes.front().Indices, Geometry->Meshes.front().Indices);
}
