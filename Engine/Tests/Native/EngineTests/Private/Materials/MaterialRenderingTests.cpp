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
#include "StandardAssetAuthoringTestSupport.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
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
			if (bRegistered) Durin::Asset::Import::Standard::UnregisterStandardAssetImportProviders();
		}

		auto Register(std::string& OutError) -> bool
		{
			bRegistered = Durin::Asset::Import::Standard::RegisterStandardAssetImportProviders(
				OutError, GetEngineTestModuleCallbackGate());
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
	ASSERT_TRUE(Durin::Tests::InstallStandardAssetAuthoringFeatures());
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
	ASSERT_TRUE(Durin::Tests::InstallStandardAssetAuthoringFeatures());
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
