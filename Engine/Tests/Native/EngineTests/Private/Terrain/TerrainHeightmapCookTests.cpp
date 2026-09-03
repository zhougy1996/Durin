#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapFactoryTestSupport.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "Components/TerrainComponent.h"
#include "Collision/CollisionGeometry.h"
#include "Physics/PhysicsTypes.h"
#include "Rendering/TerrainSceneProxy.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

TEST(FTerrainHeightmapCookTests, CookedRuntimeLoadsExactPayloadWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	std::string Error;
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TerrainHeightmapCook";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path Source = ContentRoot / "Sources/Height.raw";
	std::filesystem::create_directories(Source.parent_path());
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", ContentRoot.generic_string() + "/");
	const std::string PreviousDdc = Durin::FPaths::DerivedDataCacheDir();
	Durin::FPaths::SetDerivedDataCacheDirForTests((Root / "DDC").generic_string());
	Durin::FByteArray Raw;
	// RAW requires a square sample plane; use a 3x3 asymmetric Gaea/Unity profile.
	const std::array<uint16, 9> RawSamples{
		0, 17, 257, 4097, 32'768, 65'535, 111, 222, 333};
	Raw.clear();
	for (uint16 Sample : RawSamples)
	{
		Raw.push_back(static_cast<std::byte>(Sample));
		Raw.push_back(static_cast<std::byte>(Sample >> 8));
	}
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Raw)), Source));
	const auto Imported = Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
		Source.generic_string(), "/Game/Height");
	ASSERT_TRUE(Imported) << Imported.Message;

	const std::filesystem::path CookRoot = Root / "Cooked";
	Durin::FCookContext Cook(
		CookRoot, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Imported.Asset, "/Game/Height", Cook, Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Height", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Testing::RemoveTestWorkDirectory(Root / "DDC");
	Durin::Testing::RemoveTestWorkDirectory(ContentRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests((Root / "AbsentDDC").generic_string());
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration));
	ASSERT_TRUE(Durin::InitializeAssetManager(std::move(RuntimeConfiguration)));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DTerrainHeightmap* Cooked = nullptr;
	const Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Cooked);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Cooked, nullptr);
	const Durin::DTerrainHeightmap& ConstHeightmap = *Cooked;
	const auto BulkState = Cooked->GetCookedPlatformData().GetState();
	const auto Revision = Cooked->GetRevision();
	const auto Status = Cooked->GetStatus();
	EXPECT_EQ(ConstHeightmap.GetPayload(), nullptr);
	EXPECT_EQ(ConstHeightmap.GetPayload(), nullptr);
	EXPECT_EQ(Cooked->GetCookedPlatformData().GetState(), BulkState);
	EXPECT_EQ(Cooked->GetStatus(), Status);
	ASSERT_TRUE(Cooked->EnsurePayloadLoadedBlocking());
	ASSERT_NE(Cooked->GetPayload(), nullptr);
	EXPECT_EQ(Cooked->GetPayload()->Samples,
		std::vector<uint16>(RawSamples.begin(), RawSamples.end()));
	const auto Installed = Cooked->GetPayload();
	ASSERT_TRUE(Cooked->EnsurePayloadLoadedBlocking());
	EXPECT_EQ(Cooked->GetPayload(), Installed);
	EXPECT_EQ(Cooked->GetRevision(), Revision);
	auto* Missing = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "MissingCookedHeightmapPayload");
	EXPECT_FALSE(Missing->EnsurePayloadLoadedBlocking());
	EXPECT_EQ(Missing->GetPayload(), nullptr);
	EXPECT_EQ(Cooked->GetAssetImportData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	Cooked = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Cooked));
	EXPECT_EQ(Cooked->GetPayload(), nullptr);
	auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "CookedTerrainComponent");
	Component->SetHeightmap(Cooked);
	Durin::FCollisionGeometryRef Collision;
	Durin::FTransform CollisionTransform;
	ASSERT_TRUE(Component->BuildCollisionGeometry(Collision, CollisionTransform));
	ASSERT_NE(Cooked->GetPayload(), nullptr);
	EXPECT_EQ(Collision.GetKind(), Durin::ECollisionGeometryKind::HeightField);
	Durin::FCollisionGeometryRef SharedCollision;
	ASSERT_TRUE(Component->BuildCollisionGeometry(SharedCollision, CollisionTransform));
	EXPECT_EQ(SharedCollision.GetIdentity(), Collision.GetIdentity());
	// Collision construction is intentionally independent of renderer resource initialization.
	std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	ASSERT_NE(Proxy, nullptr);
	EXPECT_EQ(Proxy->GetKind(), Durin::EPrimitiveSceneProxyKind::Terrain);
	EXPECT_EQ(static_cast<Durin::FTerrainSceneProxy&>(*Proxy).GetPayload(), Cooked->GetPayload());
	Durin::FPhysicsQueryHit Hit;
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({0.0, 0.0, 2000.0},
		{0.0, 0.0, -2000.0}, Collision, CollisionTransform,
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDdc);
}
