#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	auto ExpectColorNear(const Durin::FVector4f& Actual, const Durin::FVector4f& Expected) -> void
	{
		EXPECT_NEAR(Actual.r, Expected.r, 1.e-6f);
		EXPECT_NEAR(Actual.g, Expected.g, 1.e-6f);
		EXPECT_NEAR(Actual.b, Expected.b, 1.e-6f);
		EXPECT_NEAR(Actual.a, Expected.a, 1.e-6f);
	}
}

TEST(FMaterialTests, InstancesInheritOverrideAndRejectParentCycles)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "BaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondInstance");

	Base->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.1, 0.2, 0.3));
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	ExpectColorNear(Second->GetRenderData().BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	First->SetScalarParameterValue(Durin::MaterialParameterOpacity, 0.4f);
	Second->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.8, 0.7, 0.6));
	ExpectColorNear(Second->GetRenderData().BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 0.4f));
	EXPECT_FALSE(First->SetParent(Second));
	EXPECT_EQ(First->GetParent(), Base);

	Durin::DestroyObject(Second);
	Durin::DestroyObject(First);
	Durin::DestroyObject(Base);
}

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData().BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Proxy.reset();
	Durin::DestroyObject(Component);
	Durin::DestroyObject(Mesh);
	Durin::DestroyObject(Material);
}

TEST(FMaterialTests, MaterialInstanceAssetsRoundTripParentAndOverrides)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Materials";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/MaterialTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Instance", InstancePath));

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	Base->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.2, 0.4, 0.6));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	Instance->SetScalarParameterValue(Durin::MaterialParameterOpacity, 0.35f);
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));

	Durin::DMaterialInstance* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(InstancePath, Loaded));
	ASSERT_NE(Loaded->GetParent(), nullptr);
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.35f));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
}
