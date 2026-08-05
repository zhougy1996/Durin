#include "MaterialTestSupport.h"
#include "Console/ConsoleCommand.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "PBRLighting.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "StandardAssetImportProviders.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Texture/TextureCubeRenderResource.h"

#include <cmath>
#include <limits>

namespace
{
	class FScopedStandardAssetImportProviders
	{
	public:
		~FScopedStandardAssetImportProviders()
		{
			if (bRegistered) Durin::UnregisterStandardAssetImportProviders();
		}

		auto Register(std::string& OutError) -> bool
		{
			bRegistered = Durin::RegisterStandardAssetImportProviders(OutError);
			return bRegistered;
		}

	private:
		bool bRegistered = false;
	};
}

TEST(FMaterialTests, DirectPBRReferenceMatchesFrozenAlignedLightValues)
{
	Durin::FPBRDirectLightingInput Input;
	Input.BaseColor = Durin::FVector3f(0.5f);
	Input.Metallic = 0.0f;
	Input.Roughness = 0.5f;
	const Durin::FVector3f Dielectric =
		Durin::EvaluatePBRDirectLighting(Input);
	EXPECT_NEAR(Dielectric.r, 0.20371833f, 1.0e-6f);
	EXPECT_NEAR(Dielectric.g, 0.20371833f, 1.0e-6f);
	EXPECT_NEAR(Dielectric.b, 0.20371833f, 1.0e-6f);

	Input.BaseColor = Durin::FVector3f(0.8f, 0.2f, 0.1f);
	Input.Metallic = 1.0f;
	const Durin::FVector3f Metal =
		Durin::EvaluatePBRDirectLighting(Input);
	EXPECT_NEAR(Metal.r, 1.01859164f, 1.0e-6f);
	EXPECT_NEAR(Metal.g, 0.25464791f, 1.0e-6f);
	EXPECT_NEAR(Metal.b, 0.12732395f, 1.0e-6f);

	Input.BaseColor = Durin::FVector3f(0.5f);
	Input.Metallic = 0.0f;
	Input.Roughness = 1.0f;
	const Durin::FVector3f RoughDielectric =
		Durin::EvaluatePBRDirectLighting(Input);
	EXPECT_NEAR(RoughDielectric.r, 0.15597184f, 1.0e-6f);
	EXPECT_NEAR(RoughDielectric.g, 0.15597184f, 1.0e-6f);
	EXPECT_NEAR(RoughDielectric.b, 0.15597184f, 1.0e-6f);
}

TEST(FMaterialTests, DirectPBRReferenceStabilizesValidatedExtremes)
{
	Durin::FPBRDirectLightingInput Input;
	Input.BaseColor = Durin::FVector3f(
		std::numeric_limits<float>::infinity(), -1.0f, 2.0f);
	Input.Metallic = std::numeric_limits<float>::quiet_NaN();
	Input.Roughness = 0.0f;
	Input.Normal = Durin::FVector3f(0.0f);
	Input.ToLight = Durin::FVector3f(0.0f);
	Input.ToView = Durin::FVector3f(0.0f);
	Input.LightRadiance = Durin::FVector3f(
		std::numeric_limits<float>::infinity());
	const Durin::FVector3f Result =
		Durin::EvaluatePBRDirectLighting(Input);
	EXPECT_TRUE(std::isfinite(Result.r));
	EXPECT_TRUE(std::isfinite(Result.g));
	EXPECT_TRUE(std::isfinite(Result.b));
	EXPECT_EQ(Result, Durin::FVector3f(0.0f));
}

TEST(FMaterialTests, MappedNormalReferencePreservesRNMAndMirroredHandedness)
{
	Durin::FPBRMappedNormalInput Input;
	Input.EncodedTextureNormal = Durin::FVector2f(0.5f, 0.75f);
	const Durin::FVector3f Ordinary = Durin::EvaluatePBRMappedNormal(Input);
	EXPECT_NEAR(Ordinary.x, 0.0f, 1.0e-6f);
	EXPECT_NEAR(Ordinary.y, 0.5f, 1.0e-6f);
	EXPECT_NEAR(Ordinary.z, 0.8660254f, 1.0e-6f);

	Input.DeterminantSign = -1.0f;
	const Durin::FVector3f Mirrored = Durin::EvaluatePBRMappedNormal(Input);
	EXPECT_NEAR(Mirrored.x, Ordinary.x, 1.0e-6f);
	EXPECT_NEAR(Mirrored.y, -Ordinary.y, 1.0e-6f);
	EXPECT_NEAR(Mirrored.z, Ordinary.z, 1.0e-6f);

	Input.ConstantTangentNormal = Durin::FVector3f(0.6f, 0.0f, 0.8f);
	Input.EncodedTextureNormal = Durin::FVector2f(0.5f, 0.5f);
	Input.DeterminantSign = 1.0f;
	const Durin::FVector3f ConstantOnly =
		Durin::EvaluatePBRMappedNormal(Input);
	EXPECT_NEAR(ConstantOnly.x, 0.6f, 1.0e-6f);
	EXPECT_NEAR(ConstantOnly.y, 0.0f, 1.0e-6f);
	EXPECT_NEAR(ConstantOnly.z, 0.8f, 1.0e-6f);
}

TEST(FMaterialTests, MappedNormalReferenceFallsBackForMissingTangentData)
{
	Durin::FPBRMappedNormalInput Input;
	Input.EncodedTextureNormal = Durin::FVector2f(1.0f, 1.0f);
	Input.GeometricNormal = Durin::FVector3f(0.0f, 1.0f, 0.0f);
	Input.Tangent = Durin::FVector3f(0.0f);
	EXPECT_EQ(
		Durin::EvaluatePBRMappedNormal(Input), Input.GeometricNormal);
}

TEST(FMaterialTests, EnvironmentPBRReferenceScopesAOToIndirectLighting)
{
	Durin::FPBREnvironmentLightingInput Input;
	Input.BaseColor = Durin::FVector3f(0.5f, 0.25f, 0.125f);
	Input.Irradiance = Durin::FVector3f(0.3f, 0.4f, 0.5f);
	Input.PrefilteredRadiance = Durin::FVector3f(0.8f, 0.7f, 0.6f);
	Input.BrdfLut = Durin::FVector2f(0.75f, 0.02f);
	const Durin::FVector3f Full = Durin::EvaluatePBREnvironmentLighting(Input);
	EXPECT_GT(Full.r, 0.0f);
	EXPECT_GT(Full.g, 0.0f);
	EXPECT_GT(Full.b, 0.0f);
	Input.AmbientOcclusion = 0.25f;
	const Durin::FVector3f Occluded = Durin::EvaluatePBREnvironmentLighting(Input);
	EXPECT_NEAR(Occluded.r, Full.r * 0.25f, 1.0e-6f);
	EXPECT_NEAR(Occluded.g, Full.g * 0.25f, 1.0e-6f);
	EXPECT_NEAR(Occluded.b, Full.b * 0.25f, 1.0e-6f);
	Input.AmbientOcclusion = 0.0f;
	EXPECT_EQ(
		Durin::EvaluatePBREnvironmentLighting(Input),
		Durin::FVector3f(0.0f));
}

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_NE(Snapshot.Proxy, nullptr);
	ASSERT_EQ(Snapshot.Materials.size(), 1u);
	ExpectColorNear(GetMaterialBinding(Snapshot.Materials[0]).BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticPropertyChangesUpdatePipelineIdentityWithoutRecreatingProxy)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "StaticIdentityMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("StaticIdentityComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 1u);

	Durin::FMaterialStaticProperties Properties;
	Properties.BlendMode = Durin::EMaterialBlendMode::Masked;
	Properties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	Properties.bTwoSided = true;
	Properties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Disabled;
	Properties.OpacityMaskThreshold = 0.6f;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));

	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Updated.Materials.size(), 1u);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	EXPECT_NE(
		Updated.Materials[0].PipelineIdentity,
		Initial.Materials[0].PipelineIdentity);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("MultiMaterialMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Snapshot.Materials.size(), 2u);
	ExpectColorNear(GetMaterialBinding(Snapshot.Materials[0]).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Snapshot.Materials[1]).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyUsesEmptyFallbackForUnassignedSlots)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::FStaticMeshTestAccess::GetMutableRenderData(Mesh)
		->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("FallbackMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Snapshot.Materials.size(), 2u);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const Durin::FMaterialRenderData& Fallback = Snapshot.Materials[SlotIndex];
		const Durin::FMaterialRenderV2Binding Binding = GetMaterialBinding(Fallback);
		EXPECT_EQ(Binding.Textures[0], nullptr);
		ExpectColorNear(
			Binding.BaseColor, Durin::FMaterialRenderV2Binding{}.BaseColor);
		EXPECT_EQ(Snapshot.MaterialProxies[SlotIndex], nullptr);
	}

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
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
	AddDebugMaterialSlot(Mesh, "Override");
	AddDebugMaterialSlot(Mesh, "SharedAgain");
	AddDebugMaterialSlot(Mesh, "Fallback");
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 1))->DefaultMaterial = Shared;
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 2))->DefaultMaterial = Shared;
	auto* Component = Harness.CreateStaticMeshComponent("SharedSlotComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterial(1, Override));
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 4u);
	ExpectColorNear(GetMaterialBinding(Initial.Materials[0]).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Initial.Materials[1]).BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Initial.Materials[2]).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(
		GetMaterialBinding(Initial.Materials[3]).BaseColor,
		Durin::FMaterialRenderV2Binding{}.BaseColor);
	EXPECT_EQ(Component->GetMaterial(2), Shared);

	Shared->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.4, 0.5, 0.6));
	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	ExpectColorNear(GetMaterialBinding(Updated.Materials[0]).BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Updated.Materials[1]).BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Updated.Materials[2]).BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(
		GetMaterialBinding(Updated.Materials[3]).BaseColor,
		Durin::FMaterialRenderV2Binding{}.BaseColor);
	EXPECT_EQ(
		Updated.Materials[0].PipelineIdentity,
		Initial.Materials[0].PipelineIdentity);
	EXPECT_EQ(Updated.MaterialProxies[0], Updated.MaterialProxies[2]);

	auto* SmallerMesh = Durin::DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(SmallerMesh);
	const FMaterialSlotsSnapshot Dormant = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Dormant.Materials.size(), 1u);
	ExpectColorNear(
		GetMaterialBinding(Dormant.Materials[0]).BaseColor,
		Durin::FMaterialRenderV2Binding{}.BaseColor);
	EXPECT_EQ(Component->GetMaterialOverride(1), Override);
	Component->SetStaticMesh(Mesh);
	const FMaterialSlotsSnapshot Reactivated = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Reactivated.Materials.size(), 4u);
	ExpectColorNear(
		GetMaterialBinding(Reactivated.Materials[1]).BaseColor,
		Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(SmallerMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Shared);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyOrdersRapidBindingChangesAndRejectsStaleRevisions)
{
	FRenderSceneHarness Harness;
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidSecondMaterial");
	auto* Replacement = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidReplacementMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	auto* Component = Harness.CreateStaticMeshComponent("RapidSlotComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	const FMaterialSlotsSnapshot Rapid = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Rapid.Materials.size(), 2u);
	ExpectColorNear(GetMaterialBinding(Rapid.Materials[0]).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(GetMaterialBinding(Rapid.Materials[1]).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	EXPECT_EQ(Rapid.ComponentRevision, Initial.ComponentRevision);

	Replacement->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.9, 0.8, 0.7));
	Component->SetMaterial(0, Replacement);
	const FMaterialSlotsSnapshot Rebound = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Rebound.Proxy, Initial.Proxy);
	EXPECT_GT(Rebound.ComponentRevision, Rapid.ComponentRevision);
	EXPECT_NE(Rebound.MaterialProxies[0], Rapid.MaterialProxies[0]);
	EXPECT_EQ(Rebound.MaterialProxies[1], Rapid.MaterialProxies[1]);
	ExpectColorNear(
		GetMaterialBinding(Rebound.Materials[0]).BaseColor,
		Durin::FVector4f(0.9f, 0.8f, 0.7f, 1.0f));

	Durin::FMaterialRenderProxyBindingUpdate Stale;
	Stale.SlotIndex = 0;
	Stale.ComponentRevision = Rapid.ComponentRevision;
	Stale.MaterialProxy = First->GetMaterialRenderProxy();
	struct FApplyStaleMaterialBindingCommand
	{
		static constexpr const char* GetName() { return "ApplyStaleMaterialBinding"; }
	};
	Durin::EnqueueRenderCommand<FApplyStaleMaterialBindingCommand>(
		[Proxy = Rebound.Proxy, Stale](Durin::FRHICommandListImmediate&) {
			Proxy->UpdateMaterialRenderProxyBinding(Stale);
		});
	const FMaterialSlotsSnapshot Ordered = CaptureMaterialSlots(Harness.Scene);
	ExpectColorNear(
		GetMaterialBinding(Ordered.Materials[0]).BaseColor,
		GetMaterialBinding(Rebound.Materials[0]).BaseColor);
	EXPECT_EQ(Ordered.ComponentRevision, Rebound.ComponentRevision);
	EXPECT_EQ(Ordered.MaterialProxies[0], Rebound.MaterialProxies[0]);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, DebugStaticMeshProvidesCompleteSplitVertexAttributes)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const auto& Positions =
		LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto& TangentsVertexBuffer =
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer;
	EXPECT_EQ(Positions.size(), 3u);
	EXPECT_EQ(
		TangentsVertexBuffer.GetNormals().size(),
		Positions.size());
	EXPECT_EQ(
		TangentsVertexBuffer.GetTangents().size(),
		Positions.size());
	EXPECT_EQ(
		LOD.VertexBuffers.ColorVertexBuffer.GetColors().size(),
		Positions.size());
	for (const auto& Channel :
		LOD.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetTexCoords())
	{
		EXPECT_EQ(Channel.size(), Positions.size());
	}
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].IndexCount, 3u);

	const Durin::FStaticMeshPackedTangentBasis PackedTangentBasis =
		Durin::PackStaticMeshTangentBasis(
			Durin::FVector3f(0.0f, 0.0f, 1.0f),
			Durin::FVector4f(1.0f, 0.0f, 0.0f, -1.0f));
	const Durin::FStaticMeshColorVertex PackedColor =
		Durin::PackStaticMeshColor(
			Durin::FVector4f(1.0f, 0.5f, 0.0f, 0.25f));
	EXPECT_EQ(PackedTangentBasis.Normal[2], 32767);
	EXPECT_EQ(PackedTangentBasis.Tangent[0], 32767);
	EXPECT_EQ(PackedTangentBasis.Tangent[3], -32767);
	EXPECT_EQ(PackedColor.Color[0], 255);
	EXPECT_EQ(PackedColor.Color[1], 128);
	EXPECT_EQ(PackedColor.Color[2], 0);
	EXPECT_EQ(PackedColor.Color[3], 64);

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, EngineMaterialPreviewMeshesAreSharedRetainedAssets)
{
	FScopedStandardAssetImportProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	for (const std::string_view PathText : {
		"/Engine/Models/Sphere",
		"/Engine/Models/Box"})
	{
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(PathText, Path));
		Durin::FRetainedEditorAsset First;
		Durin::FRetainedEditorAsset Second;
		std::string Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(Path, First, Error)) << Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(Path, Second, Error)) << Error;
		EXPECT_EQ(First.Get(), Second.Get());
		auto* Mesh = Durin::Cast<Durin::DStaticMesh>(First.Get());
		ASSERT_NE(Mesh, nullptr) << Error;
		const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		ASSERT_NE(RenderData, nullptr);
		ASSERT_EQ(RenderData->LODResources.size(), 1u);
		const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		EXPECT_GT(LOD.GetNumVertices(), 8u);
		EXPECT_GT(LOD.GetNumIndices(), 12u);
		EXPECT_EQ(LOD.NumTexCoords, 1u);
		EXPECT_EQ(
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.GetTexCoords()[0].size(),
			LOD.GetNumVertices());
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 0u);
}

TEST(FMaterialTests, MaterialPreviewDocumentsShareAssetsAcrossGarbageCollectionAndTeardown)
{
	FScopedStandardAssetImportProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	FMaterialPreviewHarness Harness;
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();

	constexpr Durin::uint64 FirstPreviewId = 987654321;
	constexpr Durin::uint64 SecondPreviewId = 987654322;
	const std::string FirstLightName = std::format("MaterialPreviewLight_{}", FirstPreviewId);
	const std::string SecondLightName = std::format("MaterialPreviewLight_{}", SecondPreviewId);
	{
		Durin::FMaterialPreview FirstPreview(FirstPreviewId);
		Durin::FMaterialPreview SecondPreview(SecondPreviewId);
		ASSERT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 2u);
		ASSERT_NE(FindObjectByName(FirstLightName), nullptr);
		ASSERT_NE(FindObjectByName(SecondLightName), nullptr);

		Durin::CollectGarbage();
		Durin::FAssetPath SpherePath;
		Durin::FAssetPath BoxPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Models/Sphere", SpherePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Models/Box", BoxPath));
		Durin::FRetainedEditorAsset SphereAsset;
		Durin::FRetainedEditorAsset BoxAsset;
		std::string Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)) << Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(BoxPath, BoxAsset, Error)) << Error;
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(SphereAsset.Get());
		auto* Box = Durin::Cast<Durin::DStaticMesh>(BoxAsset.Get());
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 0u);
	EXPECT_EQ(FindObjectByName(FirstLightName), nullptr);
	EXPECT_EQ(FindObjectByName(SecondLightName), nullptr);
}

TEST(FMaterialTests, RenderedThumbnailPreviewSceneCapturesResolvedMaterialDifferences)
{
	FScopedStandardAssetImportProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	const std::filesystem::path TextureMount =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan";
	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan.png";
	Durin::Testing::RemoveTestWorkDirectory(TextureMount);
	std::filesystem::create_directories(TextureMount);
	WriteMaterialTextureFixture(TextureSource);
	std::vector<Durin::PathUtilities::FMountPoint> MountDefinitions(
		Durin::PathUtilities::GetRegisteredMountPoints().begin(),
		Durin::PathUtilities::GetRegisteredMountPoints().end());
	MountDefinitions.push_back({
		.VirtualRoot = "/MaterialThumbnailVulkan/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.Root = TextureMount,
		.bAutoScan = true,
		.bAuthoringWritable = true});
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry(MountDefinitions);
	ASSERT_TRUE(MountRegistry.IsValid()) << MountRegistry.GetError();
	Durin::FAssetPath SpherePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::FRetainedEditorAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(
		SpherePath, PreloadedSphere, Error)) << Error;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	// A direct target run may leave deferred texture releases from earlier
	// CPU-only material cases. Drain them before creating the Vulkan device so
	// no stale game-thread owner survives into the RHI-backed portion.
	Durin::InitRenderingThread();
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	struct FBeginRenderedThumbnailFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginRenderedThumbnailFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginRenderedThumbnailFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
		});

	FMaterialTestEngine Engine;
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Engine.SetTestRendererModule(&Renderer);
	Durin::GEngine = &Engine;

	Durin::FRenderedAssetThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	Durin::DStaticMesh* CaptureMesh = nullptr;
	Durin::DStaticMesh* CaptureSphere = nullptr;
	Durin::DMaterial* CaptureMaterial = nullptr;
	Durin::DMaterialInstance* CaptureInstance = nullptr;
	Durin::DMaterialInstance* InheritedInstance = nullptr;
	Durin::DTextureCube* CaptureCube = nullptr;
	Durin::FRHITextureReferenceRef CaptureCubeReference;
	Durin::FAssetPath CaptureTexturePath;
	Durin::FAssetPath DataTexturePath;
	Durin::FAssetPath NormalTexturePath;
	Durin::FAssetPath CaptureCubePath;
	{
		Durin::FRenderedAssetThumbnailPreviewScenePool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		Durin::DStaticMesh* Sphere = Pool.GetSphereMesh();
		ASSERT_NE(Sphere, nullptr);
		CaptureSphere = Sphere;
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		CaptureMesh = Durin::DStaticMesh::CreateDebugTriangle();
		ASSERT_NE(CaptureMesh, nullptr);
		CaptureMaterial = Durin::NewObject<Durin::DMaterial>(
			nullptr, "RenderedThumbnailCaptureMaterial");
		CaptureInstance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "RenderedThumbnailCaptureInstance");
		InheritedInstance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "RenderedThumbnailInheritedInstance");
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.8, 0.15, 0.05)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.2f));
		ASSERT_TRUE(CaptureInstance->SetParent(CaptureMaterial));
		ASSERT_TRUE(CaptureInstance->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.05, 0.2, 0.8)));
		ASSERT_TRUE(CaptureInstance->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.8f));
		ASSERT_TRUE(InheritedInstance->SetParent(CaptureMaterial));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Preview", CaptureTexturePath));
		const Durin::FTexture2DImportResult TextureResult =
			Durin::DTexture2D::ImportAsset(
				TextureSource.generic_string(), CaptureTexturePath.ToString());
		ASSERT_TRUE(TextureResult) << TextureResult.Message;
		ASSERT_NE(TextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_TRUE(TextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			TextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC3_UNORM_SRGB);
		EXPECT_GT(TextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Data", DataTexturePath));
		const Durin::FTexture2DImportResult DataTextureResult =
			Durin::DTexture2D::ImportAsset(
				TextureSource.generic_string(), DataTexturePath.ToString());
		ASSERT_TRUE(DataTextureResult) << DataTextureResult.Message;
		ASSERT_TRUE(DataTextureResult.Asset->SetUsage(
			Durin::ETextureUsage::DataMask, Error)) << Error;
		ASSERT_NE(DataTextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_FALSE(DataTextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			DataTextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC7_UNORM);
		EXPECT_GT(DataTextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Normal", NormalTexturePath));
		const Durin::FTexture2DImportResult NormalTextureResult =
			Durin::DTexture2D::ImportAsset(
				TextureSource.generic_string(), NormalTexturePath.ToString());
		ASSERT_TRUE(NormalTextureResult) << NormalTextureResult.Message;
		ASSERT_TRUE(NormalTextureResult.Asset->SetUsage(
			Durin::ETextureUsage::Normal, Error)) << Error;
		ASSERT_NE(NormalTextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_FALSE(NormalTextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			NormalTextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC5_UNORM);
		EXPECT_GT(NormalTextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), TextureResult.Asset));
		Durin::FlushRenderingCommands();

		auto Capture = [&](Durin::DMaterialInterface* Material) {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(Pool.SetMaterial(
				CaptureMesh, Material, Durin::FTransform(), Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};
		const std::vector<Durin::uint8> MaterialPixels =
			Capture(CaptureMaterial);
		const std::vector<Durin::uint8> InstancePixels =
			Capture(CaptureInstance);
		const std::vector<Durin::uint8> InheritedBeforePixels =
			Capture(InheritedInstance);
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.15, 0.7, 0.2)));
		const std::vector<Durin::uint8> InheritedAfterPixels =
			Capture(InheritedInstance);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), nullptr));
		const std::vector<Durin::uint8> UntexturedPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), TextureResult.Asset));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.0, 0.0)));
		const std::vector<Durin::uint8> UV0Pixels = Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVChannel"), 3.0f));
		const std::vector<Durin::uint8> MissingUVFallbackPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(-1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(1.0, 0.0)));
		const std::vector<Durin::uint8> TransformedUVPixels =
			Capture(CaptureMaterial);
		EXPECT_EQ(UV0Pixels, MissingUVFallbackPixels);
		EXPECT_EQ(TransformedUVPixels.size(), UV0Pixels.size());
		const Durin::FMaterialRenderV2Binding TransformedUVBinding =
			GetMaterialBinding(CaptureMaterial->GetRenderData());
		EXPECT_FLOAT_EQ(TransformedUVBinding.UVChannels[0], 3.0f);
		EXPECT_EQ(
			TransformedUVBinding.UVScales[0],
			Durin::FVector2f(-1.0f, 1.0f));
		EXPECT_EQ(
			TransformedUVBinding.UVOffsets[0],
			Durin::FVector2f(1.0f, 0.0f));

		const std::array<const Durin::FName*, 8> TextureNames{
			&Durin::MaterialParameters::BaseColorTextureName(),
			&Durin::MaterialParameters::NormalTextureName(),
			&Durin::MaterialParameters::MetallicTextureName(),
			&Durin::MaterialParameters::RoughnessTextureName(),
			&Durin::MaterialParameters::AmbientOcclusionTextureName(),
			&Durin::MaterialParameters::EmissiveTextureName(),
			&Durin::MaterialParameters::OpacityTextureName(),
			&Durin::MaterialParameters::OpacityMaskTextureName()};
		const std::array<Durin::DTexture2D*, 8> RoleTextures{
			TextureResult.Asset,
			NormalTextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset,
			TextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset};
		for (size_t Role = 0; Role < TextureNames.size(); ++Role)
		{
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureNames[Role], RoleTextures[Role]));
		}
		const Durin::FMaterialRenderV2Binding MultiTextureBinding =
			GetMaterialBinding(CaptureMaterial->GetRenderData());
		for (size_t Role = 0; Role < MultiTextureBinding.Textures.size(); ++Role)
		{
			EXPECT_EQ(
				MultiTextureBinding.Textures[Role].GetReference(),
				RoleTextures[Role]->GetTextureReferenceRHI().GetReference());
		}
		const std::vector<Durin::uint8> MultiTexturePixels =
			Capture(CaptureMaterial);
		EXPECT_NE(MultiTexturePixels, UntexturedPixels);
		for (const Durin::FName* TextureName : TextureNames)
		{
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureName, nullptr));
		}
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.15, 0.7, 0.2)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.1, 0.05, 0.0)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		const Durin::FMaterialPipelineIdentity LitPipelineIdentity =
			CaptureMaterial->GetRenderData().PipelineIdentity;
		const std::vector<Durin::uint8> LitEmissivePixels =
			Capture(CaptureMaterial);
		Durin::FMaterialStaticProperties StaticProperties;
		StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		EXPECT_NE(
			CaptureMaterial->GetRenderData().PipelineIdentity,
			LitPipelineIdentity);
		const std::vector<Durin::uint8> StaticIdentityPixels =
			Capture(CaptureMaterial);
		const Durin::FConsoleCommandResult ReloadResult =
			Durin::FConsoleCommandRegistry::Get().Execute(
				"renderer.reload-shaders all");
		ASSERT_TRUE(ReloadResult.bSuccess) << ReloadResult.Message;
		const std::vector<Durin::uint8> ReloadedPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/TC_Preview", CaptureCubePath));
		const Durin::FTextureCubeImportResult CubeResult =
			Durin::DTextureCube::ImportAsset(
				Durin::Tests::GetRenderedThumbnailDirectionalCubeFaces(),
				CaptureCubePath.ToString());
		ASSERT_TRUE(CubeResult) << CubeResult.Message;
		CaptureCube = CubeResult.Asset;
		CaptureCubeReference = CaptureCube->GetTextureReferenceRHI();
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> CubePixels;
		ASSERT_TRUE(Pool.SetTextureCube(
			CubeResult.Asset, Durin::FTransform(), Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			Pool.PollCapture(CubePixels, Error),
			Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		Pool.Reset();
		ASSERT_EQ(MaterialPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(InstancePixels.size(), MaterialPixels.size());
		ASSERT_EQ(UntexturedPixels.size(), MaterialPixels.size());
		const size_t Corner = 0;
		const size_t Center = (32u * 64u + 32u) * 4u;
		EXPECT_EQ(MaterialPixels[Corner + 3], 0u);
		EXPECT_GT(MaterialPixels[Center + 3], 0u);
		EXPECT_EQ(CubePixels[Corner + 3], 255u);
		const std::array CornerRgb = {
			MaterialPixels[Corner], MaterialPixels[Corner + 1], MaterialPixels[Corner + 2]};
		const std::array MaterialCenterRgb = {
			MaterialPixels[Center], MaterialPixels[Center + 1], MaterialPixels[Center + 2]};
		const std::array InstanceCenterRgb = {
			InstancePixels[Center], InstancePixels[Center + 1], InstancePixels[Center + 2]};
		EXPECT_NE(CornerRgb, MaterialCenterRgb);
		EXPECT_NE(MaterialCenterRgb, InstanceCenterRgb);
		EXPECT_NE(InheritedBeforePixels, InheritedAfterPixels);
		EXPECT_NE(MaterialPixels, UntexturedPixels);
		EXPECT_NE(LitEmissivePixels, StaticIdentityPixels);
		EXPECT_EQ(StaticIdentityPixels, ReloadedPixels);
		EXPECT_NEAR(static_cast<int>(StaticIdentityPixels[Center + 2]), 124, 2);
		EXPECT_NEAR(static_cast<int>(StaticIdentityPixels[Center + 3]), 102, 2);
		ASSERT_EQ(CubePixels.size(), MaterialPixels.size());
		const std::array CubeCenterRgb = {
			CubePixels[Center], CubePixels[Center + 1], CubePixels[Center + 2]};
		EXPECT_NE(CubeCenterRgb, CornerRgb);
		EXPECT_NE(CubeCenterRgb, (std::array<Durin::uint8, 3>{0, 0, 0}));
		std::unordered_set<Durin::uint32> CubeCornerColors;
		for (const size_t CornerPixel : std::array<size_t, 4>{
				0,
				(64u - 1u) * 4u,
				((64u - 1u) * 64u) * 4u,
				(64u * 64u - 1u) * 4u})
		{
			CubeCornerColors.insert(
				static_cast<Durin::uint32>(CubePixels[CornerPixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[CornerPixel + 1]) << 8
				| CubePixels[CornerPixel + 2]);
		}
		EXPECT_GT(CubeCornerColors.size(), 1u);
		std::unordered_set<Durin::uint32> CubeColors;
		for (size_t Pixel = 0; Pixel < CubePixels.size(); Pixel += 4)
		{
			CubeColors.insert(
				static_cast<Durin::uint32>(CubePixels[Pixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[Pixel + 1]) << 8
				| CubePixels[Pixel + 2]);
		}
		EXPECT_GT(CubeColors.size(), 8u);

		struct FEndRenderedThumbnailFrame
		{
			static constexpr auto GetName() -> const char* { return "EndRenderedThumbnailFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndRenderedThumbnailFrame>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}

	Durin::GEngine = nullptr;
	ASSERT_NE(CaptureMesh, nullptr);
	ASSERT_NE(CaptureSphere, nullptr);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CaptureTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(DataTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(NormalTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CaptureCubePath));
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(CaptureCube);
	Durin::MarkAsGarbage(InheritedInstance);
	Durin::MarkAsGarbage(CaptureInstance);
	Durin::MarkAsGarbage(CaptureMaterial);
	Durin::MarkAsGarbage(CaptureMesh);
	PreloadedSphere = {};
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SpherePath));
	Durin::CollectGarbage();
	struct FRetireRenderedThumbnailCubeResource
	{
		static constexpr auto GetName() -> const char*
		{
			return "RetireRenderedThumbnailCubeResource";
		}
	};
	Durin::EnqueueRenderCommand<FRetireRenderedThumbnailCubeResource>(
		[Reference = std::move(CaptureCubeReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	Renderer.ShutdownModule();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	// The native suite may create another RHI in the same process; force the
	// process-wide immediate list to acquire that device's context next time.
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
}
