#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Engine/Engine.h"
#include "DObject/Class.h"
#include "LevelEditorContext.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Panels/DetailsPropertyEditing.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>

namespace
{
	class FMaterialTestEngine final : public Durin::DEngine
	{
	public:
		FMaterialTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			auto Scene = std::make_unique<Durin::FScene>();
			Durin::FScene* Result = Scene.get();
			MainScene = std::move(Scene);
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
	};

	auto WaitForRenderingThread() -> void
	{
		Durin::FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	struct FSceneSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		Durin::FMaterialRenderData Material;
		Durin::FMatrix Transform{1.0};
		Durin::uint64 ComponentRevision = 0;
		Durin::uint64 MaterialVersion = 0;
		Durin::uint64 ProxyCount = 0;
	};

	auto CaptureScene(Durin::FScene* Scene) -> FSceneSnapshot
	{
		FSceneSnapshot Snapshot;
		struct FCaptureMaterialTestSceneCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialTestScene"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialTestSceneCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate& CommandList) {
			Snapshot.ProxyCount = Scene->GetPrimitiveSceneProxies().size();
			if (Scene->GetPrimitiveSceneProxies().empty()) return;
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Scene->GetPrimitiveSceneProxies().front());
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.Material = Snapshot.Proxy->GetMaterialRenderData();
			Snapshot.Transform = Snapshot.Proxy->GetLocalToWorld();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
			Snapshot.MaterialVersion = Snapshot.Proxy->GetMaterialVersion();
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	class FRenderSceneHarness
	{
	public:
		FRenderSceneHarness()
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
			Scene = Engine.CreateTestScene();
			Durin::GEngine = &Engine;
		}

		~FRenderSceneHarness()
		{
			if (Scene != nullptr)
			{
				Scene->Release();
				WaitForRenderingThread();
				Engine.ResetTestScene();
			}
			Durin::GEngine = nullptr;
			Durin::ShutdownRenderingThread();
		}

		FMaterialTestEngine Engine;
		Durin::FScene* Scene = nullptr;
	};

	auto ExpectColorNear(const Durin::FVector4f& Actual, const Durin::FVector4f& Expected) -> void
	{
		EXPECT_NEAR(Actual.r, Expected.r, 1.e-6f);
		EXPECT_NEAR(Actual.g, Expected.g, 1.e-6f);
		EXPECT_NEAR(Actual.b, Expected.b, 1.e-6f);
		EXPECT_NEAR(Actual.a, Expected.a, 1.e-6f);
	}
}

TEST(FMaterialTests, DetailsMaterialAssignmentReplacesRegisteredProxyOnRenderThread)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstDetailsMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondDetailsMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.1, 0.2, 0.3));
	Second->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "DetailsMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(First);
	Component->RegisterComponent();
	const FSceneSnapshot Before = CaptureScene(Harness.Scene);

	Durin::FLevelEditorContext Context;
	Durin::FProperty* MaterialProperty = Component->GetClass()->FindPropertyByName("Material");
	ASSERT_NE(MaterialProperty, nullptr);
	EXPECT_TRUE(Durin::AssignDetailsObjectProperty(Context, Component, MaterialProperty, 0, Second));
	const FSceneSnapshot After = CaptureScene(Harness.Scene);

	EXPECT_NE(Before.Proxy, After.Proxy);
	ExpectColorNear(After.Material.BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::DestroyObject(Component);
	Durin::DestroyObject(Mesh);
	Durin::DestroyObject(Second);
	Durin::DestroyObject(First);
}

TEST(FMaterialTests, BoundMaterialAndParentChangesUpdateProxyInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "LiveBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveMaterialInstance");
	EXPECT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "LiveMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);

	const Durin::uint64 VersionBefore = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot ParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(ParentChanged.Proxy, Initial.Proxy);
	EXPECT_GT(Base->GetRenderStateVersion(), VersionBefore);
	EXPECT_GT(ParentChanged.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(ParentChanged.MaterialVersion, Instance->GetRenderStateVersion());
	ExpectColorNear(ParentChanged.Material.BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	const Durin::uint64 NoOpVersion = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.2, 0.4, 0.6));
	EXPECT_EQ(Base->GetRenderStateVersion(), NoOpVersion);
	Instance->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.8, 0.7, 0.6));
	Instance->ClearVectorParameterValue(Durin::MaterialParameterBaseColor);
	Base->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.9, 0.1, 0.3));
	const FSceneSnapshot Final = CaptureScene(Harness.Scene);
	EXPECT_EQ(Final.Proxy, Initial.Proxy);
	ExpectColorNear(Final.Material.BaseColor, Durin::FVector4f(0.9f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::DestroyObject(Component);
	Base->SetScalarParameterValue(Durin::MaterialParameterOpacity, 0.5f);
	Durin::DestroyObject(Mesh);
	Durin::DestroyObject(Instance);
	Durin::DestroyObject(Base);
}

TEST(FMaterialTests, SceneCommandsPreserveLatestTransformAndReleaseAllProxies)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SceneCommandComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	Component->SetWorldLocation(Durin::FVector3(4.0, 5.0, 6.0));
	const FSceneSnapshot Updated = CaptureScene(Harness.Scene);
	EXPECT_EQ(Updated.ProxyCount, 1);
	EXPECT_NEAR(Updated.Transform[3][0], 4.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][1], 5.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][2], 6.0, 1.e-6);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
	Durin::DestroyObject(Component);
	Durin::DestroyObject(Mesh);

	Harness.Scene->Release();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
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
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Proxy.reset();
	Durin::DestroyObject(Component);
	Durin::DestroyObject(Mesh);
	Durin::DestroyObject(Material);
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MultiMaterialMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(1).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Proxy.reset();
	Durin::DestroyObject(Component);
	Durin::DestroyObject(Mesh);
	Durin::DestroyObject(Second);
	Durin::DestroyObject(First);
}

TEST(FMaterialTests, DebugStaticMeshProvidesCompleteLODAndPackedAttributes)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.Positions.size(), 3u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	for (const auto& Channel : LOD.TexCoords) EXPECT_EQ(Channel.size(), LOD.Positions.size());
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].IndexCount, 3u);

	std::array<Durin::FVector2f, Durin::MaxStaticMeshUVChannels> TexCoords{};
	const Durin::FStaticMeshPackedVertex Packed = Durin::PackStaticMeshVertex(
		Durin::FVector3f(0.0f, 0.0f, 1.0f), Durin::FVector4f(1.0f, 0.0f, 0.0f, -1.0f), TexCoords, Durin::FVector4f(1.0f, 0.5f, 0.0f, 0.25f));
	EXPECT_EQ(Packed.Normal[2], 32767);
	EXPECT_EQ(Packed.Tangent[0], 32767);
	EXPECT_EQ(Packed.Tangent[3], -32767);
	EXPECT_EQ(Packed.Color[0], 255);
	EXPECT_EQ(Packed.Color[1], 128);
	EXPECT_EQ(Packed.Color[2], 0);
	EXPECT_EQ(Packed.Color[3], 64);

	Durin::DestroyObject(Mesh);
}

TEST(FMaterialTests, ImportedStaticMeshBuildsLODSectionsAndMaterialSlots)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshImports";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/MeshImportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult ImportResult = Durin::DStaticMesh::ImportAsset(Source.generic_string(), "/MeshImportTests/MultiSection");
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	const Durin::FStaticMeshRenderData* RenderData = ImportResult.Asset->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 2u);
	EXPECT_EQ(RenderData->MaterialSlots[0].Name, "Red");
	EXPECT_EQ(RenderData->MaterialSlots[1].Name, "Blue");
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.NumTexCoords, 2u);
	EXPECT_TRUE(LOD.bHasVertexColors);
	EXPECT_EQ(LOD.Positions.size(), 12u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Indices.size(), 12u);
	ASSERT_EQ(LOD.Sections.size(), 4u);
	for (size_t SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
	{
		const Durin::FStaticMeshSection& Section = LOD.Sections[SectionIndex];
		EXPECT_EQ(Section.FirstIndex, static_cast<Durin::uint32>(SectionIndex) * 3u);
		EXPECT_EQ(Section.IndexCount, 3u);
		EXPECT_EQ(Section.MaterialSlotIndex, static_cast<Durin::uint32>(SectionIndex % 2u));
		EXPECT_TRUE(Section.LocalBounds.bIsValid);
	}
	EXPECT_TRUE(LOD.LocalBounds.bIsValid);
	EXPECT_TRUE(RenderData->LocalBounds.bIsValid);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshImportTests/MultiSection", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
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
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::MaterialParameterBaseColor, Durin::FVector3(0.6, 0.4, 0.2));
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.6f, 0.4f, 0.2f, 0.35f));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
}
