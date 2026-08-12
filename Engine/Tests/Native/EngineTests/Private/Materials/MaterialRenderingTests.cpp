#include "MaterialTestSupport.h"
#include "Console/ConsoleCommand.h"
#include "DefaultTextures.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "NativeTestSupport.h"
#include "PBRLighting.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "StandardAssetImportProviders.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailPreviewScene.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

#include <array>
#include <cmath>
#include <condition_variable>
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

	class FThumbnailTestUIBackend final : public Durin::Mona::IMonaUIBackend
	{
	public:
		auto Initialize() -> void override {}
		auto Shutdown() -> void override { Registered.clear(); }
		auto NewFrame() -> void override {}
		auto Render() -> void override {}
		auto RegisterTexture(const Durin::FTextureRHIRef& Texture) -> void override
		{
			if (Texture) Registered.insert(Texture.GetReference());
		}
		auto UnregisterTexture(const Durin::FTextureRHIRef& Texture) -> void override
		{
			if (Texture) Registered.erase(Texture.GetReference());
		}
		auto IsTextureRegistered(const Durin::FRHITexture* Texture) -> bool override
		{
			return Registered.contains(Texture);
		}
		auto DrawImage(const Durin::FRHITexture* Texture, const Durin::FVector2f&) -> bool override
		{
			return IsTextureRegistered(Texture);
		}
		auto NumRegistered() const -> size_t { return Registered.size(); }

	private:
		std::unordered_set<const Durin::FRHITexture*> Registered;
	};
}

TEST(FMaterialRenderingTests, LocalLightAttenuationHasFiniteExactBoundaries)
{
	EXPECT_FLOAT_EQ(Durin::EvaluatePointLightAttenuation(1.0f, 1.0f), 0.0f);
	EXPECT_FLOAT_EQ(Durin::EvaluatePointLightAttenuation(4.0f, 1.0f), 0.0f);
	EXPECT_TRUE(std::isfinite(Durin::EvaluatePointLightAttenuation(0.0f, 10.0f)));
	EXPECT_GT(Durin::EvaluatePointLightAttenuation(0.0f, 10.0f), 0.0f);
	EXPECT_GT(Durin::EvaluatePointLightAttenuation(0.25f, 1.0f), 0.0f);
	EXPECT_FLOAT_EQ(
		Durin::EvaluateSpotLightConeAttenuation(0.5f, 1.0f, 0.5f), 0.0f);
	EXPECT_FLOAT_EQ(
		Durin::EvaluateSpotLightConeAttenuation(1.0f, 1.0f, 0.5f), 1.0f);
	EXPECT_NEAR(
		Durin::EvaluateSpotLightConeAttenuation(0.75f, 1.0f, 0.5f), 0.5f, 1.0e-6f);
	EXPECT_FLOAT_EQ(
		Durin::EvaluateSpotLightConeAttenuation(0.5f, 0.5f, 0.5f), 0.0f);
	EXPECT_FLOAT_EQ(
		Durin::EvaluateSpotLightConeAttenuation(0.5001f, 0.5f, 0.5f), 1.0f);
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

TEST(FMaterialTests, DirectPBRReferenceMatchesFrozenLowRoughnessSweep)
{
	struct FReference
	{
		float Roughness;
		float Aligned;
		float OffAxis;
	};
	constexpr std::array References{
		FReference{0.045f, 776.39996338f, 0.12223225f},
		FReference{0.1f, 31.98376846f, 0.12226272f},
		FReference{0.2f, 2.14222503f, 0.12272578f},
		FReference{0.5f, 0.20371832f, 0.13030937f},
		FReference{1.0f, 0.15597184f, 0.12506039f},
	};

	Durin::FPBRDirectLightingInput Input;
	Input.BaseColor = Durin::FVector3f(0.5f);
	Input.Metallic = 0.0f;
	for (const FReference& Reference : References)
	{
		Input.Roughness = Reference.Roughness;
		Input.ToLight = Durin::FVector3f(0.0f, 0.0f, 1.0f);
		const Durin::FVector3f Aligned =
			Durin::EvaluatePBRDirectLighting(Input);
		const float AlignedTolerance =
			std::max(1.0e-6f, Reference.Aligned * 1.0e-5f);
		EXPECT_NEAR(Aligned.r, Reference.Aligned, AlignedTolerance);
		EXPECT_NEAR(Aligned.g, Reference.Aligned, AlignedTolerance);
		EXPECT_NEAR(Aligned.b, Reference.Aligned, AlignedTolerance);

		Input.ToLight = Durin::FVector3f(0.6f, 0.0f, 0.8f);
		const Durin::FVector3f OffAxis =
			Durin::EvaluatePBRDirectLighting(Input);
		EXPECT_NEAR(OffAxis.r, Reference.OffAxis, 1.0e-6f);
		EXPECT_NEAR(OffAxis.g, Reference.OffAxis, 1.0e-6f);
		EXPECT_NEAR(OffAxis.b, Reference.OffAxis, 1.0e-6f);
	}
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

TEST(FMaterialTests, StaticMeshProxyUsesSharedEngineDefaultForUnassignedSlots)
{
	InitializeDObjectSystem();
	Durin::ResetMaterialFallbackDiagnosticsForTests();
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
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
		const Durin::FMaterialRenderData& Default = Snapshot.Materials[SlotIndex];
		const Durin::FMaterialRenderV2Binding Binding = GetMaterialBinding(Default);
		EXPECT_EQ(Binding.Textures[0], nullptr);
		ExpectColorNear(Binding.BaseColor, Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
		EXPECT_FALSE(Default.Representation.IsError());
		EXPECT_NE(Snapshot.MaterialProxies[SlotIndex], nullptr);
	}
	EXPECT_EQ(Snapshot.MaterialProxies[0], Snapshot.MaterialProxies[1]);
	EXPECT_EQ(
		Durin::GetMaterialFallbackDiagnosticsSnapshot().Get(
			Durin::EMaterialFallbackReason::UnassignedDefault),
		2u);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyResolvesPrecedenceAndUpdatesEverySharedMaterialSlot)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
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
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
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
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
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
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
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
		Durin::Editor::FRetainedAsset First;
		Durin::Editor::FRetainedAsset Second;
		std::string Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(Path, First, Error)) << Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(Path, Second, Error)) << Error;
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
	EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 0u);
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
		Durin::Editor::Material::FMaterialPreview FirstPreview(FirstPreviewId);
		Durin::Editor::Material::FMaterialPreview SecondPreview(SecondPreviewId);
		ASSERT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 2u);
		ASSERT_NE(FindObjectByName(FirstLightName), nullptr);
		ASSERT_NE(FindObjectByName(SecondLightName), nullptr);

		Durin::CollectGarbage();
		Durin::FAssetPath SpherePath;
		Durin::FAssetPath BoxPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Models/Sphere", SpherePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Models/Box", BoxPath));
		Durin::Editor::FRetainedAsset SphereAsset;
		Durin::Editor::FRetainedAsset BoxAsset;
		std::string Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)) << Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(BoxPath, BoxAsset, Error)) << Error;
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
	EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 0u);
	EXPECT_EQ(FindObjectByName(FirstLightName), nullptr);
	EXPECT_EQ(FindObjectByName(SecondLightName), nullptr);
}

TEST(FMaterialTests, RenderedThumbnailPreviewSceneCapturesResolvedMaterialDifferences)
{
	FScopedStandardAssetImportProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	InitializeDObjectSystem();
	std::string StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle StaticMeshProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider>(),
			StaticMeshProviderError);
	ASSERT_TRUE(StaticMeshProvider) << StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle MaterialProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
				Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()),
			StaticMeshProviderError);
	ASSERT_TRUE(MaterialProvider) << StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle MaterialInstanceProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
				Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
			StaticMeshProviderError);
	ASSERT_TRUE(MaterialInstanceProvider) << StaticMeshProviderError;
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
		Durin::Editor::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::Editor::FRetainedAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(
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

	Durin::Editor::FRenderedAssetThumbnailVisualContract Contract;
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
	Durin::FAssetPath StaticMeshFixturePath;
	Durin::FAssetPath StaticMeshMaterialPath;
	Durin::DStaticMesh* LowRoughnessMesh =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DMaterial* LowRoughnessMaterial =
		Durin::NewObject<Durin::DMaterial>(
			nullptr, "LowRoughnessRenderedReferenceMaterial");
	ASSERT_NE(LowRoughnessMesh, nullptr);
	ASSERT_NE(LowRoughnessMaterial, nullptr);
	ASSERT_TRUE(LowRoughnessMaterial->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.5)));
	ASSERT_TRUE(LowRoughnessMaterial->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.0f));
	Durin::Editor::FRenderedAssetThumbnailVisualContract AlignedContract = Contract;
	AlignedContract.CameraDirectionX = 0.001f;
	AlignedContract.CameraDirectionY = 0.0f;
	AlignedContract.CameraDirectionZ = 1.0f;
	AlignedContract.KeyLightDirectionX = 0.0f;
	AlignedContract.KeyLightDirectionY = 0.0f;
	AlignedContract.KeyLightDirectionZ = -1.0f;
	{
		Durin::Tests::FRenderedAssetThumbnailTestPool AlignedPool(
			AlignedContract);
		ASSERT_TRUE(AlignedPool.IsAvailable()) << AlignedPool.GetDiagnostic();
		auto CaptureAligned = [&](float Roughness) {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(LowRoughnessMaterial->SetScalarParameterValue(
				Durin::MaterialParameters::RoughnessName(), Roughness));
			EXPECT_TRUE(AlignedPool.SetMaterial(
				LowRoughnessMesh,
				LowRoughnessMaterial,
				Durin::FTransform(),
				Error)) << Error;
			EXPECT_TRUE(AlignedPool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				AlignedPool.PollCapture(Pixels, Error),
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			AlignedPool.Reset();
			return Pixels;
		};
		constexpr std::array RoughnessSweep{0.045f, 0.1f, 0.2f, 0.5f, 1.0f};
		std::array<Durin::uint32, 5> Peaks{};
		std::array<Durin::uint32, 5> SaturatedPixelCounts{};
		for (size_t Index = 0; Index < RoughnessSweep.size(); ++Index)
		{
			const std::vector<Durin::uint8> Pixels =
				CaptureAligned(RoughnessSweep[Index]);
			for (size_t Pixel = 0; Pixel < Pixels.size(); Pixel += 4)
			{
				const Durin::uint32 Brightness =
					static_cast<Durin::uint32>(Pixels[Pixel])
					+ Pixels[Pixel + 1] + Pixels[Pixel + 2];
				Peaks[Index] = std::max(Peaks[Index], Brightness);
				SaturatedPixelCounts[Index] += Brightness >= 750 ? 1u : 0u;
			}
		}
		EXPECT_GE(Peaks[0], 750u);
		EXPECT_GE(Peaks[1], 750u);
		EXPECT_GE(Peaks[2], 750u);
		EXPECT_LT(Peaks[3], 600u);
		EXPECT_LT(Peaks[4], 600u);
		EXPECT_GT(SaturatedPixelCounts[0], 0u);
		EXPECT_LT(SaturatedPixelCounts[0], SaturatedPixelCounts[1]);
		EXPECT_LT(SaturatedPixelCounts[1], SaturatedPixelCounts[2]);
		EXPECT_EQ(SaturatedPixelCounts[3], 0u);
		EXPECT_EQ(SaturatedPixelCounts[4], 0u);
	}
	{
		Durin::Tests::FRenderedAssetThumbnailTestPool Pool(Contract);
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
			Durin::StandardAssetImport::ImportTexture2DAsset(
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
			Durin::StandardAssetImport::ImportTexture2DAsset(
				TextureSource.generic_string(), DataTexturePath.ToString());
		ASSERT_TRUE(DataTextureResult) << DataTextureResult.Message;
		ASSERT_TRUE(Durin::StandardAssetImport::SetTexture2DUsage(
			*DataTextureResult.Asset, Durin::ETextureUsage::DataMask, Error)) << Error;
		ASSERT_TRUE(Durin::AssetBuild::WaitForTexture2DBuild(
			*DataTextureResult.Asset, 10.0))
			<< Durin::AssetBuild::GetTexture2DBuildDiagnostic(*DataTextureResult.Asset).Message;
		ASSERT_NE(DataTextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_FALSE(DataTextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			DataTextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC7_UNORM);
		EXPECT_GT(DataTextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Normal", NormalTexturePath));
		const Durin::FTexture2DImportResult NormalTextureResult =
			Durin::StandardAssetImport::ImportTexture2DAsset(
				TextureSource.generic_string(), NormalTexturePath.ToString());
		ASSERT_TRUE(NormalTextureResult) << NormalTextureResult.Message;
		ASSERT_TRUE(Durin::StandardAssetImport::SetTexture2DUsage(
			*NormalTextureResult.Asset, Durin::ETextureUsage::Normal, Error)) << Error;
		ASSERT_TRUE(Durin::AssetBuild::WaitForTexture2DBuild(
			*NormalTextureResult.Asset, 10.0))
			<< Durin::AssetBuild::GetTexture2DBuildDiagnostic(*NormalTextureResult.Asset).Message;
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
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
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

		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/SM_ThumbnailPreview", StaticMeshFixturePath));
		Durin::DStaticMesh* StaticMeshFixture = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(
			StaticMeshFixturePath, StaticMeshFixture)) << Error;
		ASSERT_NE(StaticMeshFixture, nullptr);
		Durin::FStaticMeshImportedData ImportedMesh;
		ImportedMesh.MaterialSlots.push_back({
			.Name = "Default",
			.SourceMaterialIndex = 0,
			.SourceName = "Default"});
		Durin::FStaticMeshImportedMesh& ImportedSection =
			ImportedMesh.Meshes.emplace_back();
		ImportedSection.Name = "ThumbnailTetrahedron";
		ImportedSection.Positions = {
			Durin::FVector3f(-0.6f, -0.5f, -0.4f),
			Durin::FVector3f(0.7f, -0.4f, -0.3f),
			Durin::FVector3f(0.0f, 0.8f, -0.2f),
			Durin::FVector3f(0.1f, 0.0f, 0.9f)};
		ImportedSection.Indices = {
			0, 2, 1,
			0, 1, 3,
			1, 2, 3,
			2, 0, 3};
		ImportedSection.SourceMaterialIndex = 0;
		ASSERT_TRUE(StaticMeshFixture->InitializeFromImportedData(
			ImportedMesh,
			{
				.SourcePath = {.Path = "/MaterialThumbnailVulkan/SM_ThumbnailPreview.fixture"},
				.SourceContentHash = "0123456789abcdef0123456789abcdef",
				.ImporterId = "MaterialThumbnailTest",
				.ImporterVersion = 1,
				.ImportSettings = Durin::FStaticMeshImportSettings::MakeDurin()},
			"StaticMesh thumbnail preview test fixture",
			Error)) << Error;
		ASSERT_TRUE(StaticMeshFixture->SetImportedDefaultMaterial(
			0, CaptureMaterial, Error)) << Error;
		StaticMeshFixture->InitResources();
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			StaticMeshFixture->GetRenderResourceStatus().Readiness,
			Durin::EStaticMeshRenderResourceReadiness::Ready);
		const std::optional<Durin::FBox> StaticMeshBounds =
			StaticMeshFixture->GetLOD0LocalBounds();
		ASSERT_TRUE(StaticMeshBounds.has_value());
		Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailView StaticMeshView;
		ASSERT_TRUE(Durin::Editor::StaticMesh::CalculateStaticMeshAssetThumbnailView({
			.LocalBounds = *StaticMeshBounds,
			.OutputAspectRatio = 1.0,
			.VerticalFieldOfViewDegrees = Contract.VerticalFieldOfViewDegrees,
			.CameraDirection = Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput{}
				.CameraDirection},
			StaticMeshView,
			Error)) << Error;
		auto CaptureStaticMesh = [&] {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(Pool.SetStaticMesh(
				StaticMeshFixture, StaticMeshView, Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(
				Error, Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailContract::bOutputOpaque))
				<< Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};
		const std::vector<Durin::uint8> StaticMeshPixels = CaptureStaticMesh();
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.85, 0.12, 0.18)));
		const std::vector<Durin::uint8> RecoloredStaticMeshPixels = CaptureStaticMesh();
		ASSERT_EQ(StaticMeshPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(RecoloredStaticMeshPixels.size(), StaticMeshPixels.size());
		EXPECT_NE(StaticMeshPixels, RecoloredStaticMeshPixels);
		Durin::uint32 GeometryPixels = 0;
		for (Durin::uint32 Y = 0; Y < 64; ++Y)
		{
			for (Durin::uint32 X = 0; X < 64; ++X)
			{
				const size_t Pixel = (Y * 64u + X) * 4u;
				GeometryPixels += StaticMeshPixels[Pixel + 3] != 0u ? 1u : 0u;
				if (X == 0 || Y == 0 || X == 63 || Y == 63)
					EXPECT_EQ(StaticMeshPixels[Pixel + 3], 0u);
			}
		}
		EXPECT_GT(GeometryPixels, 64u);
		EXPECT_LT(GeometryPixels, 64u * 64u);

		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/M_StaticMeshThumbnail",
			StaticMeshMaterialPath));
		Durin::DMaterial* StaticMeshAssetMaterial = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(
			StaticMeshMaterialPath,
			StaticMeshAssetMaterial));
		ASSERT_NE(StaticMeshAssetMaterial, nullptr);
		ASSERT_TRUE(StaticMeshAssetMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.85, 0.12, 0.18)));
		ASSERT_TRUE(Durin::Asset::SavePackage(StaticMeshAssetMaterial->GetPackage()));
		ASSERT_TRUE(StaticMeshFixture->SetImportedDefaultMaterial(
			0, StaticMeshAssetMaterial, Error)) << Error;
		ASSERT_TRUE(Durin::Asset::SavePackage(StaticMeshFixture->GetPackage()));
		ASSERT_TRUE(Durin::Asset::GetAssetRegistry().ScanMountedContent(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		const Durin::Asset::FAssetData* StaticMeshAssetData =
			Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshFixturePath);
		ASSERT_NE(StaticMeshAssetData, nullptr);
		const Durin::Editor::FAssetThumbnailPackageFingerprint StaticMeshFingerprint = {
			.VirtualPath = StaticMeshAssetData->PackagePath,
			.AssetClassName = StaticMeshAssetData->AssetClassName,
			.PackageFormatVersion = StaticMeshAssetData->FormatVersion,
			.FileSize = static_cast<Durin::uint64>(StaticMeshAssetData->FileSize),
			.LastWriteTimeTicks = StaticMeshAssetData->LastWriteTimeTicks};
		const std::filesystem::path ThumbnailCacheRoot =
			Durin::Testing::GetTestWorkDirectory() / "StaticMeshRenderedCacheVulkan";
		Durin::Testing::RemoveTestWorkDirectory(ThumbnailCacheRoot);
		ASSERT_EQ(Durin::Mona::GActiveUIBackend, nullptr);
		FThumbnailTestUIBackend ThumbnailUIBackend;
		Durin::Mona::GActiveUIBackend = &ThumbnailUIBackend;
		struct FThumbnailBackendGuard
		{
			~FThumbnailBackendGuard() { Durin::Mona::GActiveUIBackend = nullptr; }
		} ThumbnailBackendGuard;
		auto PumpCacheToReady = [&](Durin::Editor::FRenderedAssetThumbnailCache& Cache) {
			Durin::Editor::FAssetThumbnailView View;
			for (Durin::uint32 Attempt = 0; Attempt < 16; ++Attempt)
			{
				Cache.BeginFrame();
				Cache.Request(
					StaticMeshFingerprint,
					Durin::Editor::EAssetThumbnailPriority::Visible);
				View = Cache.Find(StaticMeshFixturePath);
				Cache.EndFrame();
				Durin::FlushRenderingCommands();
				if (View.State == Durin::Editor::EAssetThumbnailState::Ready
					&& View.Texture != nullptr)
					break;
			}
			return View;
		};
		{
			Durin::Editor::FRenderedAssetThumbnailCache Cache({}, {
				.CacheRoot = ThumbnailCacheRoot,
				.ObjectExtension = ".png"});
			const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(Cache);
			ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
				<< Ready.Diagnostic;
			ASSERT_NE(Ready.Texture, nullptr);
			EXPECT_TRUE(Ready.bHasTransparency);
			const Durin::Editor::FRenderedAssetThumbnailCacheStats Stats = Cache.GetStats();
			EXPECT_EQ(Stats.Pipeline.Loads, 1u);
			EXPECT_EQ(Stats.Pipeline.Renders, 1u);
			EXPECT_EQ(Stats.Pipeline.Readbacks, 1u);
			EXPECT_EQ(Stats.Pipeline.DiskHits, 0u);
			EXPECT_EQ(Stats.PreviewSceneCreations, 1u);
			EXPECT_EQ(Stats.PreviewSceneAssignments, 1u);
			EXPECT_EQ(Stats.UploadsCompleted, 1u);
			EXPECT_EQ(Stats.LiveGpuTextures, 1u);
			EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 1u);
			Cache.Clear();
			EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
			EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 0u);
		}
		{
			Durin::Editor::FRenderedAssetThumbnailCache WarmCache({}, {
				.CacheRoot = ThumbnailCacheRoot,
				.ObjectExtension = ".png"});
			const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(WarmCache);
			ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
				<< Ready.Diagnostic;
			const Durin::Editor::FRenderedAssetThumbnailCacheStats Stats = WarmCache.GetStats();
			EXPECT_EQ(Stats.Pipeline.DiskHits, 1u);
			EXPECT_EQ(Stats.Pipeline.Loads, 0u);
			EXPECT_EQ(Stats.Pipeline.Renders, 0u);
			EXPECT_EQ(Stats.Pipeline.Readbacks, 0u);
			EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
			EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
			EXPECT_EQ(Stats.UploadsCompleted, 1u);

			WarmCache.CancelPendingRequests();
			const Durin::Editor::FAssetThumbnailView Retained =
				WarmCache.Find(StaticMeshFixturePath);
			EXPECT_EQ(Retained.State, Durin::Editor::EAssetThumbnailState::Ready);
			EXPECT_EQ(Retained.Texture, Ready.Texture);
			WarmCache.BeginFrame();
			WarmCache.Request(
				StaticMeshFingerprint,
				Durin::Editor::EAssetThumbnailPriority::Visible);
			const Durin::Editor::FAssetThumbnailView Revisited =
				WarmCache.Find(StaticMeshFixturePath);
			WarmCache.EndFrame();
			EXPECT_EQ(Revisited.State, Durin::Editor::EAssetThumbnailState::Ready);
			EXPECT_EQ(Revisited.Texture, Ready.Texture);
			const Durin::Editor::FRenderedAssetThumbnailCacheStats RevisitedStats =
				WarmCache.GetStats();
			EXPECT_EQ(RevisitedStats.Pipeline.DiskHits, Stats.Pipeline.DiskHits);
			EXPECT_EQ(RevisitedStats.UploadsQueued, Stats.UploadsQueued);
			WarmCache.Clear();
		}

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
		Durin::DStaticMesh* TriangleCaptureMesh = CaptureMesh;
		CaptureMesh = CaptureSphere;
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
		EXPECT_EQ(UV0Pixels.size(), MissingUVFallbackPixels.size());
		EXPECT_EQ(TransformedUVPixels.size(), UV0Pixels.size());
		const Durin::FMaterialRenderV3Binding TransformedUVBinding =
			GetMaterialBinding(CaptureMaterial->GetRenderData());
		EXPECT_FLOAT_EQ(TransformedUVBinding.UVChannels[0], 3.0f);
		EXPECT_EQ(
			TransformedUVBinding.UVScales[0],
			Durin::FVector2f(-1.0f, 1.0f));
		EXPECT_EQ(
			TransformedUVBinding.UVOffsets[0],
			Durin::FVector2f(1.0f, 0.0f));

		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVChannel"), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.0, 0.0)));
		Durin::FMaterialSamplerState RepeatSampler;
		RepeatSampler.MinFilter = Durin::EMaterialSamplerMinFilter::Nearest;
		RepeatSampler.MagFilter = Durin::EMaterialSamplerMagFilter::Nearest;
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(RepeatSampler)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVRotation"), 1.57079633f));
		const std::vector<Durin::uint8> RotatedUVPixels = Capture(CaptureMaterial);
		EXPECT_NE(RotatedUVPixels, UV0Pixels);
		EXPECT_FLOAT_EQ(
			GetMaterialBinding(CaptureMaterial->GetRenderData()).UVRotations[0],
			1.57079633f);

		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVRotation"), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, 2.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.75, 0.75)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(RepeatSampler)));
		const std::vector<Durin::uint8> RepeatPixels = Capture(CaptureMaterial);
		Durin::FMaterialSamplerState ClampSampler = RepeatSampler;
		ClampSampler.AddressU = Durin::EMaterialSamplerAddressMode::ClampToEdge;
		ClampSampler.AddressV = Durin::EMaterialSamplerAddressMode::ClampToEdge;
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(ClampSampler)));
		const std::vector<Durin::uint8> ClampPixels = Capture(CaptureMaterial);
		EXPECT_NE(RepeatPixels, ClampPixels);
		EXPECT_EQ(
			GetMaterialBinding(CaptureMaterial->GetRenderData()).Samplers[0],
			ClampSampler);
		CaptureMesh = TriangleCaptureMesh;

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
		Durin::FMaterialRenderProxyRef TextureRoleProxy =
			CaptureMaterial->GetMaterialRenderProxy();
		for (size_t Role = 0; Role < TextureNames.size(); ++Role)
		{
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureNames[Role], RoleTextures[Role]));
			Durin::FMaterialRenderData ProxyRenderData;
			struct FCaptureTextureRoleProxyCommand
			{
				static constexpr auto GetName() -> const char*
				{
					return "CaptureTextureRoleProxy";
				}
			};
			Durin::EnqueueRenderCommand<FCaptureTextureRoleProxyCommand>(
				[TextureRoleProxy, &ProxyRenderData](
					Durin::FRHICommandListImmediate&) {
					ProxyRenderData =
						TextureRoleProxy->Resolve_RenderThread();
				});
			WaitForRenderingThread();
			const Durin::FMaterialRenderData DirectRenderData =
				CaptureMaterial->GetRenderData();
			EXPECT_EQ(
				ProxyRenderData.PipelineIdentity,
				DirectRenderData.PipelineIdentity);
			EXPECT_TRUE(std::ranges::equal(
				ProxyRenderData.Representation.GetUniformPayload(),
				DirectRenderData.Representation.GetUniformPayload()));
			EXPECT_TRUE(std::ranges::equal(
				ProxyRenderData.Representation.GetResources(),
				DirectRenderData.Representation.GetResources()));
			const Durin::FMaterialRenderV2Binding RoleBinding =
				GetMaterialBinding(ProxyRenderData);
			EXPECT_EQ(
				RoleBinding.Textures[Role].GetReference(),
				RoleTextures[Role]->GetTextureReferenceRHI().GetReference());
			for (size_t OtherRole = 0;
				OtherRole < RoleBinding.Textures.size(); ++OtherRole)
			{
				if (OtherRole != Role)
				{
					EXPECT_EQ(RoleBinding.Textures[OtherRole], nullptr);
				}
			}
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureNames[Role], nullptr));
		}
		Durin::ReleaseMaterialRenderProxy_GameThread(
			std::move(TextureRoleProxy));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.55, 0.45, 0.35)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.5f));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.0, 0.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.0)));
		const std::vector<Durin::uint8> PbrBaselinePixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 1.0f));
		const std::vector<Durin::uint8> MetallicOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.1f));
		const std::vector<Durin::uint8> RoughnessOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.5f));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.6, 0.0, 0.8)));
		const std::vector<Durin::uint8> NormalOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.0, 0.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.15, 0.05, 0.0)));
		const std::vector<Durin::uint8> EmissiveOnlyPixels =
			Capture(CaptureMaterial);
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
		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
		StaticProperties.OpacityMaskThreshold = 0.4f;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.39f));
		const std::vector<Durin::uint8> MaskedBelowPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.4f));
		const std::vector<Durin::uint8> MaskedEqualPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.41f));
		const std::vector<Durin::uint8> MaskedAbovePixels =
			Capture(CaptureMaterial);

		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.0f));
		const std::vector<Durin::uint8> TranslucentZeroPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		const std::vector<Durin::uint8> TranslucentPartialPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 1.0f));
		const std::vector<Durin::uint8> TranslucentFullPixels =
			Capture(CaptureMaterial);

		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Opaque;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 1.0f));
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

		Durin::FRHITextureReferenceRef Texture2DReference =
			TextureResult.Asset->GetTextureReferenceRHI();
		const Durin::FViewEnvironmentOverride CubeEnvironment{
			.TextureReference = CaptureCubeReference};
		const Durin::FViewEnvironmentOverride Texture2DEnvironment{
			.TextureReference = Texture2DReference};
		const Durin::uint32 CubeReferenceBaseline =
			CaptureCubeReference->GetRefCount();
		const Durin::uint32 Texture2DReferenceBaseline =
			Texture2DReference->GetRefCount();
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.SetView(Error)) << Error;
		EXPECT_EQ(
			CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u);
		ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline + 1u);
		Pool.Reset();
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline);
		Pool.Reset();
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline);
		ASSERT_TRUE(Pool.SetView(Error)) << Error;
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		EXPECT_EQ(
			CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u);
		Pool.Reset();
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> DirectEnvironmentPixels;
		ASSERT_EQ(
			Pool.PollCapture(DirectEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		ASSERT_FALSE(DirectEnvironmentPixels.empty());
		Pool.Reset();

		Durin::FTextureRHIRef OriginalCubeTarget;
		struct FRetargetRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "RetargetRenderedThumbnailEnvironment";
			}
		};
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		Durin::EnqueueRenderCommand<FRetargetRenderedThumbnailEnvironment>(
			[Reference = CaptureCubeReference, &OriginalCubeTarget](
				Durin::FRHICommandListImmediate&) {
				OriginalCubeTarget =
					Reference->GetReferencedTexture_RenderThread();
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(),
					Durin::GetDefaultCubeTexture_RenderThread());
			});
		Durin::FlushRenderingCommands();
		ASSERT_NE(OriginalCubeTarget, nullptr);
		EXPECT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> RetargetedEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(RetargetedEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		EXPECT_NE(RetargetedEnvironmentPixels, DirectEnvironmentPixels);
		Pool.Reset();

		struct FRestoreRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "RestoreRenderedThumbnailEnvironment";
			}
		};
		auto RestoreCubeTarget = [&] {
			Durin::EnqueueRenderCommand<FRestoreRenderedThumbnailEnvironment>(
				[Reference = CaptureCubeReference, OriginalCubeTarget](
					Durin::FRHICommandListImmediate&) {
					Durin::GDynamicRHI->RHIUpdateTextureReference(
						Reference.GetReference(), OriginalCubeTarget.GetReference());
				});
			Durin::FlushRenderingCommands();
		};
		RestoreCubeTarget();

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		struct FClearRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "ClearRenderedThumbnailEnvironment";
			}
		};
		Durin::EnqueueRenderCommand<FClearRenderedThumbnailEnvironment>(
			[Reference = CaptureCubeReference](
				Durin::FRHICommandListImmediate&) {
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(), nullptr);
			});
		Durin::FlushRenderingCommands();
		EXPECT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> UnavailableEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(UnavailableEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Failed);
		EXPECT_TRUE(UnavailableEnvironmentPixels.empty());
		EXPECT_NE(Error.find("view environment"), std::string::npos);
		Pool.Reset();
		RestoreCubeTarget();

		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> EmptyScenePixels;
		ASSERT_EQ(
			Pool.PollCapture(EmptyScenePixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		EXPECT_EQ(EmptyScenePixels.size(), DirectEnvironmentPixels.size());
		EXPECT_NE(EmptyScenePixels, DirectEnvironmentPixels);
		Pool.Reset();

		ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> FailedEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(FailedEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Failed);
		EXPECT_TRUE(FailedEnvironmentPixels.empty());
		EXPECT_NE(Error.find("view environment"), std::string::npos);
		Pool.Reset();

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		struct FRenderedThumbnailCancellationGate
		{
			std::mutex Mutex;
			std::condition_variable Condition;
			bool bEntered = false;
			bool bReleased = false;
		};
		struct FBlockRenderedThumbnailCaptureForCancellation
		{
			static constexpr auto GetName() -> const char*
			{
				return "BlockRenderedThumbnailCaptureForCancellation";
			}
		};
		const auto Gate = std::make_shared<FRenderedThumbnailCancellationGate>();
		Durin::EnqueueRenderCommand<FBlockRenderedThumbnailCaptureForCancellation>(
			[Gate](Durin::FRHICommandListImmediate&) {
				std::unique_lock Lock(Gate->Mutex);
				Gate->bEntered = true;
				Gate->Condition.notify_all();
				Gate->Condition.wait(Lock, [&Gate] { return Gate->bReleased; });
			});
		{
			std::unique_lock Lock(Gate->Mutex);
			Gate->Condition.wait(Lock, [&Gate] { return Gate->bEntered; });
		}
		const bool bCancelledCaptureStarted = Pool.BeginCapture(Error);
		Pool.Reset();
		const Durin::uint32 QueuedReferenceCount =
			CaptureCubeReference->GetRefCount();
		{
			std::lock_guard Lock(Gate->Mutex);
			Gate->bReleased = true;
		}
		Gate->Condition.notify_all();
		Durin::FlushRenderingCommands();
		EXPECT_TRUE(bCancelledCaptureStarted) << Error;
		EXPECT_GT(QueuedReferenceCount, CubeReferenceBaseline);
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
		std::vector<Durin::uint8> CancelledPixels;
		EXPECT_EQ(
			Pool.PollCapture(CancelledPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Idle);
		EXPECT_TRUE(CancelledPixels.empty());
		EXPECT_TRUE(Error.empty());

		std::vector<Durin::uint8> CubePixels;
		ASSERT_TRUE(Pool.SetTextureCube(CubeResult.Asset, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			Pool.PollCapture(CubePixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
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
		EXPECT_NE(PbrBaselinePixels, MetallicOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, RoughnessOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, NormalOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, EmissiveOnlyPixels);
		EXPECT_NE(LitEmissivePixels, StaticIdentityPixels);
		EXPECT_EQ(StaticIdentityPixels, ReloadedPixels);
		EXPECT_NEAR(static_cast<int>(StaticIdentityPixels[Center + 2]), 124, 2);
		EXPECT_NEAR(static_cast<int>(StaticIdentityPixels[Center + 3]), 102, 2);
		EXPECT_EQ(MaskedBelowPixels[Center + 3], 0u);
		EXPECT_GT(MaskedEqualPixels[Center + 3], 0u);
		EXPECT_EQ(MaskedEqualPixels, MaskedAbovePixels);
		EXPECT_EQ(TranslucentZeroPixels[Center + 3], 0u);
		EXPECT_NEAR(
			static_cast<int>(TranslucentPartialPixels[Center + 3]), 102, 2);
		EXPECT_EQ(TranslucentFullPixels[Center + 3], 255u);
		EXPECT_LT(
			TranslucentPartialPixels[Center], TranslucentFullPixels[Center]);
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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StaticMeshFixturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StaticMeshMaterialPath));
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(CaptureCube);
	Durin::MarkAsGarbage(InheritedInstance);
	Durin::MarkAsGarbage(CaptureInstance);
	Durin::MarkAsGarbage(CaptureMaterial);
	Durin::MarkAsGarbage(CaptureMesh);
	Durin::MarkAsGarbage(LowRoughnessMaterial);
	Durin::MarkAsGarbage(LowRoughnessMesh);
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
