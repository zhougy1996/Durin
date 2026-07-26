#include "MaterialTestSupport.h"

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
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
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyUsesEmptyFallbackForUnassignedSlots)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "FallbackMeshComponent");
	Component->SetStaticMesh(Mesh);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const Durin::FMaterialRenderData& Fallback = StaticMeshProxy->GetMaterialRenderData(SlotIndex);
		ExpectColorNear(Fallback.BaseColor, Durin::FMaterialRenderData{}.BaseColor);
		EXPECT_EQ(Fallback.BaseColorTexture, nullptr);
		EXPECT_FLOAT_EQ(Fallback.SpecularStrength, Durin::FMaterialRenderData{}.SpecularStrength);
		EXPECT_FLOAT_EQ(Fallback.Shininess, Durin::FMaterialRenderData{}.Shininess);
	}

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyResolvesPrecedenceAndUpdatesEverySharedMaterialSlot)
{
	FRenderSceneHarness Harness;
	auto* Shared = Durin::NewObject<Durin::DMaterial>(nullptr, "SharedSlotMaterial");
	auto* Override = Durin::NewObject<Durin::DMaterial>(nullptr, "OverrideSlotMaterial");
	Shared->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	Override->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = Shared;
	const Durin::FGuid OverrideId = AddDebugMaterialSlot(Mesh, "Override");
	const Durin::FGuid SharedId = AddDebugMaterialSlot(Mesh, "SharedAgain");
	AddDebugMaterialSlot(Mesh, "Fallback");
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 1))->DefaultMaterial = Shared;
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 2))->DefaultMaterial = Shared;
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SharedSlotComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterialBySlotId(OverrideId, Override));
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 4u);
	ExpectColorNear(Initial.Materials[0].BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(Initial.Materials[1].BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(Initial.Materials[2].BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(Initial.Materials[3].BaseColor, Durin::FMaterialRenderData{}.BaseColor);
	EXPECT_EQ(Component->GetMaterialBySlotId(SharedId), Shared);

	Shared->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.4, 0.5, 0.6));
	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_GT(Updated.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(Updated.Materials[0].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[1].BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[2].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[3].BaseColor, Durin::FMaterialRenderData{}.BaseColor);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Shared);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyOrdersRapidCrossSlotUpdatesAndRejectsStaleRevisions)
{
	FRenderSceneHarness Harness;
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidSecondMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "RapidSlotComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	const FMaterialSlotsSnapshot Rapid = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Rapid.Materials.size(), 2u);
	ExpectColorNear(Rapid.Materials[0].BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(Rapid.Materials[1].BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Durin::FMaterialRenderUpdate Newer;
	Newer.SlotIndex = 0;
	Newer.ComponentRevision = Rapid.ComponentRevision + 2;
	Newer.RenderData.BaseColor = Durin::FVector4f(0.9f, 0.8f, 0.7f, 1.0f);
	Durin::FMaterialRenderUpdate Stale = Newer;
	Stale.ComponentRevision = Rapid.ComponentRevision + 1;
	Stale.RenderData.BaseColor = Durin::FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
	struct FApplyOrderedMaterialUpdatesCommand
	{
		static constexpr const char* GetName() { return "ApplyOrderedMaterialUpdates"; }
	};
	Durin::EnqueueRenderCommand<FApplyOrderedMaterialUpdatesCommand>(
		[Proxy = Rapid.Proxy, Newer, Stale](Durin::FRHICommandListImmediate&) {
			Proxy->UpdateMaterialRenderData(Newer);
			Proxy->UpdateMaterialRenderData(Stale);
		});
	const FMaterialSlotsSnapshot Ordered = CaptureMaterialSlots(Harness.Scene);
	ExpectColorNear(Ordered.Materials[0].BaseColor, Newer.RenderData.BaseColor);
	EXPECT_EQ(Ordered.ComponentRevision, Newer.ComponentRevision);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
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

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, EngineMaterialPreviewMeshesLoadAsTransientGeometry)
{
	InitializeDObjectSystem();
	const std::string PreviewContent = Durin::FPaths::EngineContentDir() + "Editor/MaterialPreview/";
	for (const std::string_view Name : {"Sphere", "Box"})
	{
		std::string Error;
		Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateTransientFromFile(
			PreviewContent + std::string(Name) + ".obj", nullptr, std::format("TestMaterialPreview{}", Name), Error);
		ASSERT_NE(Mesh, nullptr) << Error;
		const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		ASSERT_NE(RenderData, nullptr);
		ASSERT_EQ(RenderData->LODResources.size(), 1u);
		const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		EXPECT_GT(LOD.Positions.size(), 8u);
		EXPECT_GT(LOD.Indices.size(), 12u);
		EXPECT_EQ(LOD.NumTexCoords, 1u);
		EXPECT_EQ(LOD.TexCoords[0].size(), LOD.Positions.size());
		Durin::MarkAsGarbage(Mesh);
	}
	Durin::CollectGarbage();
}

TEST(FMaterialTests, MaterialPreviewResourcesRemainAliveAcrossGarbageCollection)
{
	FMaterialPreviewHarness Harness;

	constexpr Durin::uint64 PreviewId = 987654321;
	const std::string SphereName = std::format("MaterialPreviewSphere_{}", PreviewId);
	const std::string BoxName = std::format("MaterialPreviewBox_{}", PreviewId);
	const std::string LightName = std::format("MaterialPreviewLight_{}", PreviewId);
	{
		Durin::FMaterialPreview Preview(PreviewId);
		ASSERT_NE(FindObjectByName(SphereName), nullptr);
		ASSERT_NE(FindObjectByName(BoxName), nullptr);
		ASSERT_NE(FindObjectByName(LightName), nullptr);

		Durin::CollectGarbage();
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(FindObjectByName(SphereName));
		auto* Box = Durin::Cast<Durin::DStaticMesh>(FindObjectByName(BoxName));
		auto* Light = Durin::Cast<Durin::DDirectionalLightComponent>(FindObjectByName(LightName));
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		ASSERT_NE(Light, nullptr);
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage();
	EXPECT_EQ(FindObjectByName(SphereName), nullptr);
	EXPECT_EQ(FindObjectByName(BoxName), nullptr);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
}
