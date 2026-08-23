#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/LightSceneProxy.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RendererModule.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Terrain/TerrainHeightmap.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
namespace
{
	constexpr Durin::uint32 TimingWidth = 1920;
	constexpr Durin::uint32 TimingHeight = 1080;
	constexpr Durin::uint32 WarmupFrames = 30;
	constexpr Durin::uint32 MeasuredFrames = 120;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GSceneTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GGBufferTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GDeferredTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GRetainedOpaqueTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GVolumetricCloudTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GSortedTranslucencyTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GPostProcessTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GShadowTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GContactTimingQueries = nullptr;
	Durin::FContactShadowVisibilityRenderer::ERoute GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::FactorOne;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionFilterTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionResolveTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>*
		GGroundTruthAmbientOcclusionFeatureTimingQueries = nullptr;
	std::vector<std::byte>* GGroundTruthAmbientOcclusionPixels = nullptr;
	std::vector<std::byte>* GGroundTruthAmbientOcclusionFilteredPixels = nullptr;
	std::vector<std::byte>* GSpecularAASurfacePixels = nullptr;
	Durin::FViewRenderCounters GLastCounters;

	struct FGBufferQualificationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "GBufferQualification";
		}
	};

	auto CaptureGBufferTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGBufferTimingQueries != nullptr)
			GGBufferTimingQueries->push_back(Query);
	}

	auto CaptureSceneTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GSceneTimingQueries != nullptr)
			GSceneTimingQueries->push_back(Query);
	}

	auto CaptureDeferredTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GDeferredTimingQueries != nullptr)
			GDeferredTimingQueries->push_back(Query);
	}

	auto CaptureRetainedOpaqueTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GRetainedOpaqueTimingQueries != nullptr)
			GRetainedOpaqueTimingQueries->push_back(Query);
	}

	auto CaptureVolumetricCloudTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GVolumetricCloudTimingQueries != nullptr)
			GVolumetricCloudTimingQueries->push_back(Query);
	}

	auto CaptureSortedTranslucencyTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GSortedTranslucencyTimingQueries != nullptr)
			GSortedTranslucencyTimingQueries->push_back(Query);
	}

	auto CapturePostProcessTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GPostProcessTimingQueries != nullptr)
			GPostProcessTimingQueries->push_back(Query);
	}

	auto CaptureShadowTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GShadowTimingQueries != nullptr)
			GShadowTimingQueries->push_back(Query);
	}

	auto CaptureContactTiming(const Durin::FGPUTimingQueryRHIRef& Query,
		Durin::FContactShadowVisibilityRenderer::ERoute Route) -> void
	{
		if (GContactTimingQueries != nullptr && Route == GExpectedContactRoute)
			GContactTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GGroundTruthAmbientOcclusionTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionFilterTiming(
		const Durin::FGPUTimingQueryRHIRef& Query
	) -> void
	{
		if (GGroundTruthAmbientOcclusionFilterTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionFilterTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionResolveTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGroundTruthAmbientOcclusionResolveTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionResolveTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusionFeatureTiming(
		const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GGroundTruthAmbientOcclusionFeatureTimingQueries != nullptr)
			GGroundTruthAmbientOcclusionFeatureTimingQueries->push_back(Query);
	}

	auto CaptureGroundTruthAmbientOcclusion(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* RawVisibility,
		bool bFiltered
	) -> void
	{
		auto* Pixels = bFiltered ? GGroundTruthAmbientOcclusionFilteredPixels : GGroundTruthAmbientOcclusionPixels;
		if (Pixels == nullptr) return;
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
							  "GroundTruthAmbientOcclusionReadback",
							  RawVisibility->GetSizeX(), RawVisibility->GetSizeY(),
							  RawVisibility->GetFormat()
		)
							  .SetFlags(Durin::ETextureCreateFlags::DestinationCopy | Durin::ETextureCreateFlags::CPUReadback | Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1
		};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{RawVisibility, Whole, Durin::ERHIAccess::GraphicsShaderRead, Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::Discard, Durin::ERHIAccess::TransferWrite}
		});
		CommandList.CopyTexture(RawVisibility, Readback, std::array{Durin::FRHITextureCopyRegion{.Extent = {RawVisibility->GetSizeX(), RawVisibility->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{RawVisibility, Whole, Durin::ERHIAccess::TransferRead, Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{Readback, Whole, Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}
		});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0,
			*Pixels
		));
	}

	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
	}

	auto CaptureSpecularAAGBuffer(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture*,
		Durin::FRHITexture*,
		Durin::FRHITexture* Surface,
		Durin::FRHITexture*,
		Durin::FRHITexture*) -> void
	{
		if (GSpecularAASurfacePixels == nullptr) return;
		const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
			"SpecularAASurfaceReadback", Surface->GetSizeX(), Surface->GetSizeY(),
			Surface->GetFormat())
			.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback
				| Durin::ETextureCreateFlags::ShaderResource);
		Durin::FTextureRHIRef Readback =
			Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Readback, nullptr);
		const Durin::FRHITextureSubresourceRange Whole{
			Durin::ERHITextureAspect::Color, 0, 1, 0, 1};
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Surface, Whole,
				Durin::ERHIAccess::GraphicsShaderRead,
				Durin::ERHIAccess::TransferRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::Discard,
				Durin::ERHIAccess::TransferWrite}});
		CommandList.CopyTexture(
			Surface, Readback,
			std::array{Durin::FRHITextureCopyRegion{
				.Extent = {Surface->GetSizeX(), Surface->GetSizeY(), 1}}});
		CommandList.TransitionTextures(std::array{
			Durin::FRHITextureTransition{Surface, Whole,
				Durin::ERHIAccess::TransferRead,
				Durin::ERHIAccess::GraphicsShaderRead},
			Durin::FRHITextureTransition{Readback, Whole,
				Durin::ERHIAccess::TransferWrite,
				Durin::ERHIAccess::GraphicsShaderRead}});
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			CommandList, Readback, 0, 0, *GSpecularAASurfacePixels));
	}

	auto MakeStaticQuad() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		LOD.VertexBuffers.PositionVertexBuffer.Init({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1
		);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4
		);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({.Name = "Opaque", .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSpecularAAQuad() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = MakeStaticQuad();
		auto& Vertices = Data->LODResources[0].VertexBuffers;
		Vertices.PositionVertexBuffer.Init({
			{-0.25f, -0.25f, 0.0f}, {0.25f, -0.25f, 0.0f},
			{0.25f, 0.25f, 0.0f}, {-0.25f, 0.25f, 0.0f}});
		Vertices.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			{{0.0f, 0.0f, 1.0f}, {0.8f, 0.0f, 0.6f},
			 {0.8f, 0.0f, 0.6f}, {0.0f, 0.0f, 1.0f}},
			std::vector<Durin::FVector4f>(
				4, {0.0f, 1.0f, 0.0f, 1.0f}));
		Data->LODResources[0].Sections[0].LocalBounds =
			Durin::FBox({-0.25, -0.25, 0.0}, {0.25, 0.25, 0.0});
		Data->LODResources[0].LocalBounds =
			Data->LODResources[0].Sections[0].LocalBounds;
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSkeletalQuad() -> std::unique_ptr<Durin::FSkeletalMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FSkeletalMeshRenderData>();
		Data->VertexBuffers.Geometry.PositionVertexBuffer.Init({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1
		);
		Data->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4
		);
		Durin::FSkeletalMeshVertexInfluences Influence;
		Influence.BoneIndices[0] = 0;
		Influence.Weights[0] = 1.0f;
		Influence.Count = 1;
		Data->VertexBuffers.InfluenceVertexBuffer.Init(
			std::vector<Durin::FSkeletalMeshVertexInfluences>(4, Influence)
		);
		Data->IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		Data->Sections.push_back({.Name = Durin::FName("Opaque"), .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		Data->MaterialSlots = {Durin::FName("Opaque")};
		Data->PaletteBoneIndices = {0};
		Data->InverseBindMatrices = {Durin::FMatrix4f(1.0f)};
		Data->InfluenceBounds = {
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})
		};
		Data->LocalBounds = Data->Sections[0].LocalBounds;
		return Data;
	}
} // namespace

TEST(FGBufferQualificationTests, FourFamilyPassMeetsFrozenRTX3090TimingAndMemoryGates)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::FRHIInitializationContext::Headless());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("GBufferQualificationTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto StaticQuad = MakeStaticQuad();
	auto SpecularAAQuad = MakeSpecularAAQuad();
	auto SkeletalQuad = MakeSkeletalQuad();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(StaticQuad->InitResources(CommandList));
			ASSERT_TRUE(SpecularAAQuad->InitResources(CommandList));
			ASSERT_TRUE(SkeletalQuad->InitResources(CommandList));
		}
	);
	Durin::FlushRenderingCommands();

	auto Material = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
	Durin::FMaterialRenderProxyPublication Publication;
	Publication.LocalVersion = 1;
	Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
		.BlendMode = Durin::EMaterialBlendMode::Opaque,
		.ShadingModel = Durin::EMaterialShadingModel::Lit,
		.bTwoSided = true
	};
	Publication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = {0.7, 0.4, 0.2}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
	Durin::FlushRenderingCommands();

	Durin::FScene Scene;
	auto Translate = [](double X, double Y) {
		return Durin::Math::TranslationMatrix(Durin::FVector3{X, Y, 0.0});
	};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), Translate(-1.0, -1.0));
	Durin::FSplineMeshRenderDynamicData SplineData{
		.Params = {},
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}),
		.Revision = 1
	};
	SplineData.Params.EndPosition = {1.0, 0.0, 0.0};
	SplineData.Params.StartTangent = {1.0, 0.0, 0.0};
	SplineData.Params.EndTangent = {1.0, 0.0, 0.0};
	SplineData.Params.SourceForwardMin = 0.0;
	SplineData.Params.SourceForwardMax = 1.0;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2), std::make_unique<Durin::FSplineMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, SplineData), Translate(0.0, -1.0));
	auto Pose = std::make_shared<Durin::FSkeletalPosePalette>();
	Pose->Revision = 1;
	Pose->SkeletonCompatibilityIdentity = "GBufferQualification";
	Pose->Matrices = {Durin::FMatrix4f(1.0f)};
	Pose->LocalBounds = SkeletalQuad->LocalBounds;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3), std::make_unique<Durin::FSkeletalMeshSceneProxy>(SkeletalQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, Pose), Translate(-1.0, 0.0));
	const std::array<Durin::uint16, 4> Heights{};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> HeightPayload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		2, 2, Heights, HeightPayload, Error
	)) << Error;
	Durin::FTerrainPatchDescriptor Patch{
		.OriginX = 0, .OriginY = 0, .CellCountX = 1, .CellCountY = 1, .LODSteps = {1}, .LODErrors = {0.0}, .LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})
	};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(4), std::make_unique<Durin::FTerrainSceneProxy>(HeightPayload, 1, 1.0, 1.0, 0.0, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch}, Patch.LocalBounds, Material, 1), Durin::FMatrix(1.0));
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FPointLightSceneData PointA;
	PointA.Position = {-1.5, -0.75, 2.0};
	PointA.Color = {1.0f, 0.15f, 0.05f};
	PointA.Intensity = 5.0f;
	PointA.Range = 6.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(20), std::make_unique<Durin::FPointLightSceneProxy>(PointA));
	Durin::FSpotLightSceneData SpotA;
	SpotA.Position = {1.5, -0.75, 3.0};
	SpotA.Direction = {-0.15, 0.1, -1.0};
	SpotA.Color = {0.05f, 0.2f, 1.0f};
	SpotA.Intensity = 8.0f;
	SpotA.Range = 8.0f;
	SpotA.InnerConeAngle = 25.0f;
	SpotA.OuterConeAngle = 40.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(21), std::make_unique<Durin::FSpotLightSceneProxy>(SpotA));
	auto PointB = PointA;
	PointB.Position = {-0.5, 1.25, 1.5};
	PointB.Color = {0.1f, 1.0f, 0.2f};
	PointB.Intensity = 3.0f;
	PointB.Range = 4.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(22), std::make_unique<Durin::FPointLightSceneProxy>(PointB));
	auto SpotB = SpotA;
	SpotB.Position = {1.25, 1.25, 2.5};
	SpotB.Direction = {0.1, -0.2, -1.0};
	SpotB.Color = {1.0f, 0.6f, 0.1f};
	SpotB.Intensity = 6.0f;
	SpotB.Range = 7.0f;
	SpotB.InnerConeAngle = 20.0f;
	SpotB.OuterConeAngle = 45.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(23), std::make_unique<Durin::FSpotLightSceneProxy>(SpotB));
	Durin::FlushRenderingCommands();

	Durin::FScene SpecularAAScene;
	SpecularAAScene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(200),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			SpecularAAQuad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	auto CaptureSpecularAASurface = [&Renderer, &SpecularAAScene](
		bool bEnableSpecularAA, std::vector<std::byte>& Pixels) {
		GSpecularAASurfacePixels = &Pixels;
		Durin::SetGBufferCaptureSink(CaptureSpecularAAGBuffer);
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &SpecularAAScene, bEnableSpecularAA](
				Durin::FRHICommandListImmediate& CommandList) {
				constexpr Durin::uint32 Width = 32;
				constexpr Durin::uint32 Height = 32;
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					"SpecularAAQualification", Width, Height,
					Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewProjectionMatrix = Durin::FMatrix(1.0);
				View.ViewportWidth = Width;
				View.ViewportHeight = Height;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.Mode.bEnableSpecularAA = bEnableSpecularAA;
				Durin::FSceneViewRenderOptions Options;
				Options.bEnableGBufferQualification = true;
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(
					Renderer.RenderView(
						CommandList, &SpecularAAScene, View, Target, false,
						Options),
					Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
		Durin::SetGBufferCaptureSink(nullptr);
		GSpecularAASurfacePixels = nullptr;
	};
	std::vector<std::byte> SpecularAADisabledSurface;
	std::vector<std::byte> SpecularAAEnabledSurface;
	CaptureSpecularAASurface(false, SpecularAADisabledSurface);
	CaptureSpecularAASurface(true, SpecularAAEnabledSurface);
	ASSERT_EQ(SpecularAADisabledSurface.size(), 32u * 32u * 4u);
	ASSERT_EQ(SpecularAAEnabledSurface.size(), SpecularAADisabledSurface.size());
	size_t ValidPixels = 0;
	size_t BroadenedPixels = 0;
	for (size_t Offset = 0; Offset < SpecularAADisabledSurface.size(); Offset += 4)
	{
		if (SpecularAADisabledSurface[Offset + 3] == std::byte{0}) continue;
		++ValidPixels;
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 1],
			SpecularAADisabledSurface[Offset + 1]);
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 2],
			SpecularAADisabledSurface[Offset + 2]);
		EXPECT_EQ(
			SpecularAAEnabledSurface[Offset + 3],
			SpecularAADisabledSurface[Offset + 3]);
		EXPECT_GE(
			std::to_integer<Durin::uint8>(SpecularAAEnabledSurface[Offset]),
			std::to_integer<Durin::uint8>(SpecularAADisabledSurface[Offset]));
		if (SpecularAAEnabledSurface[Offset]
			> SpecularAADisabledSurface[Offset]) ++BroadenedPixels;
	}
	EXPECT_GT(ValidPixels, 0u);
	EXPECT_GT(BroadenedPixels, 0u);

	std::vector<Durin::FGPUTimingQueryRHIRef> GBufferQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> DeferredQueries;
	GGBufferTimingQueries = &GBufferQueries;
	GDeferredTimingQueries = &DeferredQueries;
	Durin::SetGBufferTimingQuerySink(CaptureGBufferTiming);
	Durin::SetDeferredDirectionalTimingQuerySink(CaptureDeferredTiming);
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GBufferQualificationColor", TimingWidth, TimingHeight,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = TimingWidth;
			View.ViewportHeight = TimingHeight;
			// Keep the qualification target isolated from the production deferred
			// interval while exercising the same Lit material records.
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.DirectionalShadow.Candidate =
				Durin::EDirectionalShadowCandidate::SingleMap;
			View.Settings.DirectionalShadow.FilterQuality =
				Durin::EDirectionalShadowFilterQuality::Medium;
			Durin::FSceneViewRenderOptions Options;
			Options.bEnableGBufferQualification = true;
			Options.bEnableDeferredDirectionalQualification = true;
			for (Durin::uint32 Frame = 0;
				 Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	Durin::SetGBufferTimingQuerySink(nullptr);
	Durin::SetDeferredDirectionalTimingQuerySink(nullptr);
	GGBufferTimingQueries = nullptr;
	GDeferredTimingQueries = nullptr;
	for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		auto AllReady = [](const auto& Queries) {
			return std::ranges::all_of(Queries, [](const auto& Query) {
				return Query->GetResult().State
					   == Durin::ERHIGPUTimingResultState::Ready;
			});
		};
		const bool bReady =
			GBufferQueries.size() == WarmupFrames + MeasuredFrames
			&& DeferredQueries.size() == WarmupFrames + MeasuredFrames
			&& AllReady(GBufferQueries) && AllReady(DeferredQueries);
		if (bReady) break;
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(GBufferQueries.size(), WarmupFrames + MeasuredFrames);
	ASSERT_EQ(DeferredQueries.size(), WarmupFrames + MeasuredFrames);
	std::vector<Durin::uint64> GBufferDurations;
	std::vector<Durin::uint64> DeferredDurations;
	std::vector<Durin::uint64> CombinedDurations;
	for (size_t Index = WarmupFrames; Index < GBufferQueries.size(); ++Index)
	{
		const Durin::FRHIGPUTimingResult GBufferResult =
			GBufferQueries[Index]->GetResult();
		const Durin::FRHIGPUTimingResult DeferredResult =
			DeferredQueries[Index]->GetResult();
		ASSERT_EQ(GBufferResult.State, Durin::ERHIGPUTimingResultState::Ready);
		ASSERT_EQ(DeferredResult.State, Durin::ERHIGPUTimingResultState::Ready);
		GBufferDurations.push_back(GBufferResult.DurationNanoseconds);
		DeferredDurations.push_back(DeferredResult.DurationNanoseconds);
		CombinedDurations.push_back(
			GBufferResult.DurationNanoseconds
			+ DeferredResult.DurationNanoseconds
		);
	}
	std::ranges::sort(GBufferDurations);
	std::ranges::sort(DeferredDurations);
	std::ranges::sort(CombinedDurations);
	ASSERT_EQ(GBufferDurations.size(), MeasuredFrames);
	auto Median = [](const std::vector<Durin::uint64>& Durations) {
		const size_t Upper = Durations.size() / 2;
		const size_t Lower = (Durations.size() - 1) / 2;
		return (Durations[Lower] + Durations[Upper]) / 2;
	};
	auto Percentile95 = [](const std::vector<Durin::uint64>& Durations) {
		return Durations[(Durations.size() * 95u + 99u) / 100u - 1u];
	};
	const Durin::uint64 GBufferMedian = Median(GBufferDurations);
	const Durin::uint64 GBufferP95 = Percentile95(GBufferDurations);
	const Durin::uint64 DeferredMedian = Median(DeferredDurations);
	const Durin::uint64 DeferredP95 = Percentile95(DeferredDurations);
	const Durin::uint64 CombinedMedian = Median(CombinedDurations);
	const Durin::uint64 CombinedP95 = Percentile95(CombinedDurations);
	EXPECT_LE(GBufferMedian, 350'000u);
	EXPECT_LE(GBufferP95, 500'000u);
	EXPECT_LE(DeferredMedian, 450'000u);
	EXPECT_LE(DeferredP95, 650'000u);
	EXPECT_LE(CombinedMedian, 800'000u);
	EXPECT_LE(CombinedP95, 1'000'000u);
	EXPECT_EQ(GLastCounters.GBufferAttemptedDraws, 4u);
	EXPECT_EQ(GLastCounters.GBufferSuccessfulDraws, 4u);
	EXPECT_EQ(GLastCounters.GBufferRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSkippedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferStaticMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.SelectedDirectionalLights, 1u);
	EXPECT_EQ(GLastCounters.SelectedPointLights, 2u);
	EXPECT_EQ(GLastCounters.SelectedSpotLights, 2u);
	EXPECT_EQ(GLastCounters.OverflowPointLights, 0u);
	EXPECT_EQ(GLastCounters.OverflowSpotLights, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalUnavailableViews, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalOutputBytes, Durin::FDeferredDirectionalLightingRenderer::CalculateTargetBytes(TimingWidth, TimingHeight));
	const Durin::uint64 AttachmentBytes =
		Durin::FGBufferRenderer::CalculateTargetBytes(TimingWidth, TimingHeight);
	EXPECT_EQ(GLastCounters.GBufferAttachmentBytes, AttachmentBytes);
	EXPECT_LE(AttachmentBytes, Durin::FGBufferRenderer::MaximumRetainedBytes);
	std::cout << "Deferred lighting qualification: adapter=NVIDIA GeForce RTX 3090, "
			  << "resolution=1920x1080, warmup=" << WarmupFrames
			  << ", samples=" << MeasuredFrames
			  << ", gbuffer_median_ns=" << GBufferMedian
			  << ", gbuffer_p95_ns=" << GBufferP95
			  << ", deferred_median_ns=" << DeferredMedian
			  << ", deferred_p95_ns=" << DeferredP95
			  << ", combined_median_ns=" << CombinedMedian
			  << ", combined_p95_ns=" << CombinedP95
			  << ", gbuffer_bytes=" << AttachmentBytes
			  << ", deferred_bytes="
			  << GLastCounters.DeferredDirectionalOutputBytes
			  << ", active_route_bytes="
			  << 107'827'200u << "\n";
	GBufferQueries.clear();
	DeferredQueries.clear();
	std::vector<Durin::FGPUTimingQueryRHIRef> AmbientOcclusionQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> AmbientOcclusionFilterQueries;
	GGroundTruthAmbientOcclusionTimingQueries = &AmbientOcclusionQueries;
	GGroundTruthAmbientOcclusionFilterTimingQueries =
		&AmbientOcclusionFilterQueries;
	Durin::SetGroundTruthAmbientOcclusionTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionTiming
	);
	Durin::SetGroundTruthAmbientOcclusionFilterTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionFilterTiming
	);
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GroundTruthAmbientOcclusionQualification", TimingWidth, TimingHeight,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = TimingWidth;
			View.ViewportHeight = TimingHeight;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.AmbientOcclusion.Quality =
				Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
			Durin::FSceneViewRenderOptions Options;
			Options.bEnableGroundTruthAmbientOcclusionQualification = true;
			for (Durin::uint32 Frame = 0;
				 Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	Durin::SetGroundTruthAmbientOcclusionTimingQuerySink(nullptr);
	Durin::SetGroundTruthAmbientOcclusionFilterTimingQuerySink(nullptr);
	GGroundTruthAmbientOcclusionTimingQueries = nullptr;
	GGroundTruthAmbientOcclusionFilterTimingQueries = nullptr;
	for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		const bool bReady =
			AmbientOcclusionQueries.size() == WarmupFrames + MeasuredFrames
			&& AmbientOcclusionFilterQueries.size()
				   == WarmupFrames + MeasuredFrames
			&& std::ranges::all_of(AmbientOcclusionQueries, [](const auto& Query) {
				   return Query->GetResult().State
						  == Durin::ERHIGPUTimingResultState::Ready;
			   })
			&& std::ranges::all_of(AmbientOcclusionFilterQueries, [](const auto& Query) {
				   return Query->GetResult().State
						  == Durin::ERHIGPUTimingResultState::Ready;
			   });
		if (bReady) break;
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(AmbientOcclusionQueries.size(), WarmupFrames + MeasuredFrames);
	ASSERT_EQ(AmbientOcclusionFilterQueries.size(), WarmupFrames + MeasuredFrames);
	std::vector<Durin::uint64> AmbientOcclusionDurations;
	std::vector<Durin::uint64> AmbientOcclusionFilterDurations;
	std::vector<Durin::uint64> AmbientOcclusionCombinedDurations;
	for (size_t Index = WarmupFrames; Index < AmbientOcclusionQueries.size(); ++Index)
	{
		const auto Result = AmbientOcclusionQueries[Index]->GetResult();
		ASSERT_EQ(Result.State, Durin::ERHIGPUTimingResultState::Ready);
		AmbientOcclusionDurations.push_back(Result.DurationNanoseconds);
		const auto FilterResult =
			AmbientOcclusionFilterQueries[Index]->GetResult();
		ASSERT_EQ(FilterResult.State, Durin::ERHIGPUTimingResultState::Ready);
		AmbientOcclusionFilterDurations.push_back(
			FilterResult.DurationNanoseconds
		);
		AmbientOcclusionCombinedDurations.push_back(
			Result.DurationNanoseconds + FilterResult.DurationNanoseconds
		);
	}
	std::ranges::sort(AmbientOcclusionDurations);
	std::ranges::sort(AmbientOcclusionFilterDurations);
	std::ranges::sort(AmbientOcclusionCombinedDurations);
	const Durin::uint64 AmbientOcclusionMedian = Median(AmbientOcclusionDurations);
	const Durin::uint64 AmbientOcclusionP95 =
		Percentile95(AmbientOcclusionDurations);
	const Durin::uint64 AmbientOcclusionFilterMedian =
		Median(AmbientOcclusionFilterDurations);
	const Durin::uint64 AmbientOcclusionFilterP95 =
		Percentile95(AmbientOcclusionFilterDurations);
	const Durin::uint64 AmbientOcclusionCombinedMedian =
		Median(AmbientOcclusionCombinedDurations);
	const Durin::uint64 AmbientOcclusionCombinedP95 =
		Percentile95(AmbientOcclusionCombinedDurations);
	EXPECT_GT(AmbientOcclusionMedian, 0u);
	EXPECT_LE(AmbientOcclusionMedian, 600'000u);
	EXPECT_GT(AmbientOcclusionFilterMedian, 0u);
	EXPECT_LE(AmbientOcclusionFilterMedian, 250'000u);
	EXPECT_LE(AmbientOcclusionCombinedMedian, 850'000u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionUnavailableViews, 0u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionRawPassFailures, 0u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionFilterPassFailures, 0u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionResolvePassFailures, 0u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionFullResolutionViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionActiveBytes, 4'147'200u);
	std::cout << "GTAO_RAW_QUALIFICATION gpu=NVIDIA_GeForce_RTX_3090"
			  << ",driver=591.86,vulkan=1.4.325"
			  << ",configuration=Win64-Debug-DurinEditor,validation=enabled"
			  << ",resolution=1920x1080,warmup_frames=" << WarmupFrames
			  << ",measured_frames=" << MeasuredFrames
			  << ",median_ns=" << AmbientOcclusionMedian
			  << ",p95_ns=" << AmbientOcclusionP95
			  << ",filter_median_ns=" << AmbientOcclusionFilterMedian
			  << ",filter_p95_ns=" << AmbientOcclusionFilterP95
			  << ",combined_median_ns=" << AmbientOcclusionCombinedMedian
			  << ",combined_p95_ns=" << AmbientOcclusionCombinedP95
			  << ",active_bytes="
			  << GLastCounters.GroundTruthAmbientOcclusionActiveBytes << '\n';
	AmbientOcclusionQueries.clear();
	AmbientOcclusionFilterQueries.clear();

	constexpr Durin::uint32 CaptureWidth = 320;
	constexpr Durin::uint32 CaptureHeight = 180;
	auto CaptureAmbientOcclusionFrame = [&Renderer, &Scene](
											Durin::uint32 Width, Durin::uint32 Height,
											std::vector<std::byte>& Pixels,
											std::vector<std::byte>& FilteredPixels
										) {
		GGroundTruthAmbientOcclusionPixels = &Pixels;
		GGroundTruthAmbientOcclusionFilteredPixels = &FilteredPixels;
		Durin::SetGroundTruthAmbientOcclusionCaptureSink(
			CaptureGroundTruthAmbientOcclusion
		);
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &Scene, Width, Height](
				Durin::FRHICommandListImmediate& CommandList
			) {
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  "GroundTruthAmbientOcclusionCapture", Width, Height,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				// Look down world -Z using Durin's X-forward view convention.
				View.ViewLocation = {0.0, 0.0, 1.0};
				View.ViewMatrix = Durin::FMatrix(0.0);
				View.ViewMatrix[2][0] = -1.0;
				View.ViewMatrix[3][0] = 1.0;
				View.ViewMatrix[0][1] = 1.0;
				View.ViewMatrix[1][2] = -1.0;
				View.ViewMatrix[3][3] = 1.0;
				constexpr double NearClip = 0.1;
				constexpr double FarClip = 10.0;
				const double YScale = 1.0 / std::tan(Durin::Math::DegreesToRadians(60.0) * 0.5);
				View.ProjectionMatrix = Durin::FMatrix(0.0);
				View.ProjectionMatrix[1][0] = YScale;
				View.ProjectionMatrix[2][1] = -YScale;
				View.ProjectionMatrix[0][2] =
					FarClip / (FarClip - NearClip);
				View.ProjectionMatrix[3][2] =
					-NearClip * FarClip / (FarClip - NearClip);
				View.ProjectionMatrix[0][3] = 1.0;
				View.ViewProjectionMatrix =
					View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = Width;
				View.ViewportHeight = Height;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.AmbientOcclusion.Quality =
					Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
				Durin::FSceneViewRenderOptions Options;
				Options.bEnableGroundTruthAmbientOcclusionQualification = true;
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		Durin::FlushRenderingCommands();
		Durin::SetGroundTruthAmbientOcclusionCaptureSink(nullptr);
		GGroundTruthAmbientOcclusionPixels = nullptr;
		GGroundTruthAmbientOcclusionFilteredPixels = nullptr;
	};
	std::vector<std::byte> FirstRawVisibility;
	std::vector<std::byte> FirstFilteredVisibility;
	std::vector<std::byte> ResizedRawVisibility;
	std::vector<std::byte> ResizedFilteredVisibility;
	std::vector<std::byte> RepeatedRawVisibility;
	std::vector<std::byte> RepeatedFilteredVisibility;
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		FirstRawVisibility, FirstFilteredVisibility
	);
	CaptureAmbientOcclusionFrame(
		384, 216, ResizedRawVisibility, ResizedFilteredVisibility
	);
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		RepeatedRawVisibility, RepeatedFilteredVisibility
	);
	ASSERT_EQ(FirstRawVisibility.size(), CaptureWidth * CaptureHeight);
	ASSERT_EQ(FirstFilteredVisibility.size(), CaptureWidth * CaptureHeight);
	ASSERT_EQ(ResizedRawVisibility.size(), 384u * 216u);
	ASSERT_EQ(ResizedFilteredVisibility.size(), 384u * 216u);
	EXPECT_EQ(RepeatedRawVisibility, FirstRawVisibility);
	EXPECT_EQ(RepeatedFilteredVisibility, FirstFilteredVisibility);
	auto PixelAt = [&FirstRawVisibility](Durin::uint32 X, Durin::uint32 Y) {
		return std::to_integer<Durin::uint32>(
			FirstRawVisibility[Y * CaptureWidth + X]);
	};
	EXPECT_GE(PixelAt(80, 45), 250u);
	EXPECT_GE(PixelAt(240, 45), 250u);
	EXPECT_GE(PixelAt(80, 135), 250u);
	EXPECT_GE(PixelAt(240, 135), 250u);
	auto FilteredPixelAt = [&FirstFilteredVisibility](
							   Durin::uint32 X, Durin::uint32 Y
						   ) {
		return std::to_integer<Durin::uint32>(
			FirstFilteredVisibility[Y * CaptureWidth + X]);
	};
	EXPECT_GE(FilteredPixelAt(80, 45), 250u);
	EXPECT_GE(FilteredPixelAt(240, 45), 250u);
	EXPECT_GE(FilteredPixelAt(80, 135), 250u);
	EXPECT_GE(FilteredPixelAt(240, 135), 250u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionActiveBytes, 2u * CaptureWidth * CaptureHeight);

	Durin::FMatrix RaisedTransform = Durin::Math::TranslationMatrix(
		Durin::FVector3{-0.25, -0.25, 0.25}
	);
	RaisedTransform = Durin::Math::Scale(
		RaisedTransform, Durin::FVector3{0.5, 0.5, 1.0});
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(7), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1), RaisedTransform);
	Durin::FlushRenderingCommands();
	std::vector<std::byte> RaisedContactVisibility;
	std::vector<std::byte> RaisedContactFilteredVisibility;
	CaptureAmbientOcclusionFrame(
		CaptureWidth, CaptureHeight,
		RaisedContactVisibility, RaisedContactFilteredVisibility
	);
	ASSERT_EQ(RaisedContactVisibility.size(), FirstRawVisibility.size());
	ASSERT_EQ(RaisedContactFilteredVisibility.size(), FirstFilteredVisibility.size());
	size_t OccludedPixels = 0;
	for (size_t Index = 0; Index < RaisedContactVisibility.size(); ++Index)
	{
		if (std::to_integer<Durin::uint32>(RaisedContactVisibility[Index]) + 2u
			< std::to_integer<Durin::uint32>(FirstRawVisibility[Index]))
			++OccludedPixels;
	}
	EXPECT_GT(OccludedPixels, 0u);
	EXPECT_LT(OccludedPixels, CaptureWidth * CaptureHeight);
	size_t FilteredOccludedPixels = 0;
	for (size_t Index = 0; Index < RaisedContactFilteredVisibility.size(); ++Index)
	{
		if (std::to_integer<Durin::uint32>(RaisedContactFilteredVisibility[Index])
				+ 2u
			< std::to_integer<Durin::uint32>(FirstFilteredVisibility[Index]))
			++FilteredOccludedPixels;
	}
	EXPECT_GT(FilteredOccludedPixels, 0u);
	EXPECT_LT(FilteredOccludedPixels, CaptureWidth * CaptureHeight);
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(7));
	Durin::FlushRenderingCommands();

	auto UnlitMaterial = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
	Durin::FMaterialRenderProxyPublication UnlitPublication;
	UnlitPublication.LocalVersion = 1;
	UnlitPublication.LocalLayer.StaticProperties =
		Durin::FMaterialStaticProperties{
			.BlendMode = Durin::EMaterialBlendMode::Opaque,
			.ShadingModel = Durin::EMaterialShadingModel::Unlit
		};
	UnlitPublication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = {0.1, 0.8, 0.3}});
	ASSERT_TRUE(UnlitMaterial->QueuePublication_GameThread(
		std::move(UnlitPublication)
	));
	auto TranslucentMaterial = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
	Durin::FMaterialRenderProxyPublication TranslucentPublication;
	TranslucentPublication.LocalVersion = 1;
	TranslucentPublication.LocalLayer.StaticProperties =
		Durin::FMaterialStaticProperties{
			.BlendMode = Durin::EMaterialBlendMode::Translucent,
			.ShadingModel = Durin::EMaterialShadingModel::Unlit
		};
	TranslucentPublication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = {0.8, 0.2, 0.5}});
	TranslucentPublication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::OpacityId, .Type = Durin::EMaterialParameterType::Scalar, .ScalarValue = 0.45});
	ASSERT_TRUE(TranslucentMaterial->QueuePublication_GameThread(
		std::move(TranslucentPublication)
	));
	Durin::FlushRenderingCommands();
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(5), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{UnlitMaterial}, 1), Translate(-0.55, -0.45));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(6), std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(), std::vector<Durin::FMaterialRenderProxyRef>{TranslucentMaterial}, 1), Translate(-0.35, -0.25));
	Durin::FlushRenderingCommands();

	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionGBufferQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionSceneQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionDeferredQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionRetainedOpaqueQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionVolumetricCloudQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef>
		ProductionSortedTranslucencyQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionPostProcessQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionShadowQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> ProductionContactQueries;
	std::vector<Durin::uint64> ProductionGBufferDurations;
	std::vector<Durin::uint64> ProductionSceneDurations;
	std::vector<Durin::uint64> ProductionDeferredDurations;
	std::vector<Durin::uint64> ProductionDeferredWithoutAODurations;
	std::vector<Durin::uint64> ProductionRetainedOpaqueDurations;
	std::vector<Durin::uint64> ProductionVolumetricCloudDurations;
	std::vector<Durin::uint64> ProductionSortedTranslucencyDurations;
	std::vector<Durin::uint64> ProductionRetainedDurations;
	std::vector<Durin::uint64> ProductionPostProcessDurations;
	std::vector<Durin::uint64> ProductionShadowDurations;
	std::vector<Durin::uint64> ProductionComputeContactDurations;
	std::vector<Durin::uint64> ProductionFragmentContactDurations;
	std::vector<Durin::uint64> ConstrainedComputeContactDurations;
	std::vector<Durin::uint64> ConstrainedFragmentContactDurations;
	bool bEnableProductionAmbientOcclusion = true;
	bool bEnableProductionContactShadows = false;
	bool bForceProductionFragmentContact = false;
	Durin::uint32 ProductionWidth = TimingWidth;
	Durin::uint32 ProductionHeight = TimingHeight;
	Durin::uint32 ProductionViewportX = 0;
	Durin::uint32 ProductionViewportY = 0;
	Durin::uint32 ProductionViewportWidth = TimingWidth;
	Durin::uint32 ProductionViewportHeight = TimingHeight;
	Durin::EGroundTruthAmbientOcclusionQuality ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution;
	auto RenderProductionFrames = [&Renderer, &Scene,
								   &bEnableProductionAmbientOcclusion,
								   &bEnableProductionContactShadows,
								   &bForceProductionFragmentContact,
								   &ProductionWidth, &ProductionHeight,
								   &ProductionViewportX, &ProductionViewportY,
								   &ProductionViewportWidth,
								   &ProductionViewportHeight,
								   &ProductionAmbientOcclusionQuality](
			Durin::uint32 FrameCount) {
		Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
			[&Renderer, &Scene, bEnableProductionAmbientOcclusion,
			 bEnableProductionContactShadows, bForceProductionFragmentContact,
			 ProductionWidth, ProductionHeight, ProductionViewportX,
				 ProductionViewportY, ProductionViewportWidth,
				 ProductionViewportHeight,
				 ProductionAmbientOcclusionQuality, FrameCount](
				Durin::FRHICommandListImmediate& CommandList
			) {
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
									  "HybridProductionQualification", ProductionWidth, ProductionHeight,
									  Durin::EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				View.ViewProjectionMatrix = Durin::FMatrix(1.0);
				View.ViewportX = ProductionViewportX;
				View.ViewportY = ProductionViewportY;
				View.ViewportWidth = ProductionViewportWidth;
				View.ViewportHeight = ProductionViewportHeight;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.Candidate =
					Durin::EDirectionalShadowCandidate::SingleMap;
				View.Settings.DirectionalShadow.FilterQuality =
					Durin::EDirectionalShadowFilterQuality::Medium;
				View.Settings.PostProcess.bEnableFXAA = true;
				View.Settings.AmbientOcclusion.bEnabled =
					bEnableProductionAmbientOcclusion;
				View.Settings.DirectionalShadow.bEnableContactShadows =
					bEnableProductionContactShadows;
				View.Settings.AmbientOcclusion.Quality =
					ProductionAmbientOcclusionQuality;
				Durin::FSceneViewRenderOptions Options;
				Options.bForceFragmentContactVisibility =
					bForceProductionFragmentContact;
				for (Durin::uint32 Frame = 0; Frame < FrameCount; ++Frame)
				{
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			}
		);
		Durin::FlushRenderingCommands();
	};
	auto WaitForProductionQueries = [](const auto& Queries,
			Durin::uint32 ExpectedCount) {
		for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
		{
			const bool bReady = Queries.size() == ExpectedCount
								&& std::ranges::all_of(Queries, [](const auto& Query) {
									   return Query->GetResult().State
											  == Durin::ERHIGPUTimingResultState::Ready;
								   });
			if (bReady) break;
			Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
				[](Durin::FRHICommandListImmediate& CommandList) {
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			Durin::FlushRenderingCommands();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	};
	auto CollectProductionDurations = [&WaitForProductionQueries](
			auto& Queries, auto& Durations, Durin::uint32 WarmupCount,
			Durin::uint32 MeasuredCount) {
		const Durin::uint32 ExpectedCount = WarmupCount + MeasuredCount;
		WaitForProductionQueries(Queries, ExpectedCount);
		EXPECT_EQ(Queries.size(), ExpectedCount);
		if (Queries.size() == ExpectedCount)
		{
			for (size_t Index = WarmupCount; Index < Queries.size(); ++Index)
			{
				const auto Result = Queries[Index]->GetResult();
				EXPECT_EQ(Result.State,
					Durin::ERHIGPUTimingResultState::Ready);
				Durations.push_back(Result.DurationNanoseconds);
			}
		}
		Queries.clear();
	};
	auto ProfileProductionInterval = [&RenderProductionFrames,
									  &CollectProductionDurations](
			auto& Queries, auto& Durations, auto& QuerySlot,
			auto SetSink, auto CaptureSink) {
		QuerySlot = &Queries;
		SetSink(CaptureSink);
		RenderProductionFrames(WarmupFrames + MeasuredFrames);
		SetSink(nullptr);
		QuerySlot = nullptr;
		CollectProductionDurations(
			Queries, Durations, WarmupFrames, MeasuredFrames);
	};
	auto ProfileProductionRoute = [&RenderProductionFrames,
								 &CollectProductionDurations,
								 &ProductionGBufferQueries,
								 &ProductionGBufferDurations,
								 &ProductionSceneQueries,
								 &ProductionSceneDurations,
								 &ProductionDeferredQueries,
								 &ProductionDeferredDurations,
								 &ProductionRetainedOpaqueQueries,
								 &ProductionRetainedOpaqueDurations,
								 &ProductionVolumetricCloudQueries,
								 &ProductionVolumetricCloudDurations,
								 &ProductionSortedTranslucencyQueries,
								 &ProductionSortedTranslucencyDurations,
								 &ProductionPostProcessQueries,
								 &ProductionPostProcessDurations,
								 &ProductionShadowQueries,
								 &ProductionShadowDurations]() {
		GGBufferTimingQueries = &ProductionGBufferQueries;
		GSceneTimingQueries = &ProductionSceneQueries;
		GDeferredTimingQueries = &ProductionDeferredQueries;
		GRetainedOpaqueTimingQueries = &ProductionRetainedOpaqueQueries;
		GVolumetricCloudTimingQueries = &ProductionVolumetricCloudQueries;
		GSortedTranslucencyTimingQueries =
			&ProductionSortedTranslucencyQueries;
		GPostProcessTimingQueries = &ProductionPostProcessQueries;
		GShadowTimingQueries = &ProductionShadowQueries;
		Durin::SetGBufferTimingQuerySink(CaptureGBufferTiming);
		Durin::SetSceneColorTimingQuerySink(CaptureSceneTiming);
		Durin::SetDeferredDirectionalTimingQuerySink(CaptureDeferredTiming);
		Durin::SetRetainedOpaqueTimingQuerySink(CaptureRetainedOpaqueTiming);
		Durin::SetVolumetricCloudTimingQuerySink(CaptureVolumetricCloudTiming);
		Durin::SetSortedTranslucencyTimingQuerySink(
			CaptureSortedTranslucencyTiming);
		Durin::SetPostProcessTimingQuerySink(CapturePostProcessTiming);
		Durin::SetShadowDepthTimingQuerySink(CaptureShadowTiming);
		RenderProductionFrames(WarmupFrames + MeasuredFrames);
		Durin::SetShadowDepthTimingQuerySink(nullptr);
		Durin::SetPostProcessTimingQuerySink(nullptr);
		Durin::SetSortedTranslucencyTimingQuerySink(nullptr);
		Durin::SetVolumetricCloudTimingQuerySink(nullptr);
		Durin::SetRetainedOpaqueTimingQuerySink(nullptr);
		Durin::SetDeferredDirectionalTimingQuerySink(nullptr);
		Durin::SetSceneColorTimingQuerySink(nullptr);
		Durin::SetGBufferTimingQuerySink(nullptr);
		GShadowTimingQueries = nullptr;
		GPostProcessTimingQueries = nullptr;
		GSortedTranslucencyTimingQueries = nullptr;
		GVolumetricCloudTimingQueries = nullptr;
		GRetainedOpaqueTimingQueries = nullptr;
		GDeferredTimingQueries = nullptr;
		GSceneTimingQueries = nullptr;
		GGBufferTimingQueries = nullptr;
		CollectProductionDurations(
			ProductionGBufferQueries, ProductionGBufferDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionSceneQueries, ProductionSceneDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionDeferredQueries, ProductionDeferredDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionRetainedOpaqueQueries,
			ProductionRetainedOpaqueDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionVolumetricCloudQueries,
			ProductionVolumetricCloudDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionSortedTranslucencyQueries,
			ProductionSortedTranslucencyDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionPostProcessQueries, ProductionPostProcessDurations,
			WarmupFrames, MeasuredFrames);
		CollectProductionDurations(
			ProductionShadowQueries, ProductionShadowDurations,
			WarmupFrames, MeasuredFrames);
	};

	// Capture nested production intervals from the same frames before the
	// feature-isolation sweeps change GPU clock and thermal state.
	ProfileProductionRoute();
	const Durin::FViewRenderCounters ProductionRouteCounters = GLastCounters;
	bEnableProductionAmbientOcclusion = false;
	ProfileProductionInterval(
		ProductionDeferredQueries, ProductionDeferredWithoutAODurations,
		GDeferredTimingQueries,
		Durin::SetDeferredDirectionalTimingQuerySink,
		CaptureDeferredTiming);
	bEnableProductionAmbientOcclusion = true;
	std::vector<Durin::FGPUTimingQueryRHIRef> GTAOFeatureQueries;
	std::vector<Durin::FGPUTimingQueryRHIRef> GTAOResolveQueries;
	std::vector<Durin::uint64> FullGTAOFeatureDurations;
	std::vector<Durin::uint64> HalfGTAOFeatureDurations;
	std::vector<Durin::uint64> HalfGTAOResolveDurations;
	ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::FullResolution;
	ProfileProductionInterval(GTAOFeatureQueries, FullGTAOFeatureDurations,
		GGroundTruthAmbientOcclusionFeatureTimingQueries,
		Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink,
		CaptureGroundTruthAmbientOcclusionFeatureTiming);
	ProductionAmbientOcclusionQuality =
		Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution;
	GGroundTruthAmbientOcclusionFeatureTimingQueries = &GTAOFeatureQueries;
	GGroundTruthAmbientOcclusionResolveTimingQueries = &GTAOResolveQueries;
	Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionFeatureTiming);
	Durin::SetGroundTruthAmbientOcclusionResolveTimingQuerySink(
		CaptureGroundTruthAmbientOcclusionResolveTiming);
	RenderProductionFrames(WarmupFrames + MeasuredFrames);
	Durin::SetGroundTruthAmbientOcclusionResolveTimingQuerySink(nullptr);
	Durin::SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(nullptr);
	GGroundTruthAmbientOcclusionResolveTimingQueries = nullptr;
	GGroundTruthAmbientOcclusionFeatureTimingQueries = nullptr;
	CollectProductionDurations(GTAOFeatureQueries,
		HalfGTAOFeatureDurations, WarmupFrames, MeasuredFrames);
	CollectProductionDurations(GTAOResolveQueries,
		HalfGTAOResolveDurations, WarmupFrames, MeasuredFrames);
	bEnableProductionContactShadows = true;
	bForceProductionFragmentContact = false;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Compute;
	ProfileProductionInterval(ProductionContactQueries,
		ProductionComputeContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderCounters ProductionComputeContactCounters =
		GLastCounters;
	bForceProductionFragmentContact = true;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment;
	ProfileProductionInterval(ProductionContactQueries,
		ProductionFragmentContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderCounters ProductionFragmentContactCounters =
		GLastCounters;
	ProductionWidth = 1919;
	ProductionHeight = 1079;
	ProductionViewportX = 137;
	ProductionViewportY = 89;
	ProductionViewportWidth = 1601;
	ProductionViewportHeight = 901;
	bForceProductionFragmentContact = false;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Compute;
	ProfileProductionInterval(ProductionContactQueries,
		ConstrainedComputeContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderCounters ConstrainedComputeContactCounters =
		GLastCounters;
	bForceProductionFragmentContact = true;
	GExpectedContactRoute =
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment;
	ProfileProductionInterval(ProductionContactQueries,
		ConstrainedFragmentContactDurations, GContactTimingQueries,
		Durin::FContactShadowVisibilityRenderer::SetTimingQuerySink,
		CaptureContactTiming);
	const Durin::FViewRenderCounters ConstrainedFragmentContactCounters =
		GLastCounters;
	ProductionWidth = TimingWidth;
	ProductionHeight = TimingHeight;
	ProductionViewportX = 0;
	ProductionViewportY = 0;
	ProductionViewportWidth = TimingWidth;
	ProductionViewportHeight = TimingHeight;
	bEnableProductionContactShadows = false;
	bForceProductionFragmentContact = false;
	std::vector<Durin::uint64> ProductionTotalDurations;
	ASSERT_EQ(
		ProductionGBufferDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionSceneDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionDeferredDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionDeferredWithoutAODurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionRetainedOpaqueDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionVolumetricCloudDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionSortedTranslucencyDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionPostProcessDurations.size(), MeasuredFrames);
	ASSERT_EQ(
		ProductionShadowDurations.size(), MeasuredFrames);
	ASSERT_EQ(FullGTAOFeatureDurations.size(), MeasuredFrames);
	ASSERT_EQ(HalfGTAOFeatureDurations.size(), MeasuredFrames);
	ASSERT_EQ(HalfGTAOResolveDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionComputeContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ProductionFragmentContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ConstrainedComputeContactDurations.size(), MeasuredFrames);
	ASSERT_EQ(ConstrainedFragmentContactDurations.size(), MeasuredFrames);
	ProductionRetainedDurations.reserve(MeasuredFrames);
	for (size_t Index = 0; Index < MeasuredFrames; ++Index)
	{
		ProductionRetainedDurations.push_back(
			ProductionRetainedOpaqueDurations[Index]
			+ ProductionVolumetricCloudDurations[Index]
			+ ProductionSortedTranslucencyDurations[Index]);
		EXPECT_GE(ProductionSceneDurations[Index],
			ProductionDeferredDurations[Index]
				+ ProductionRetainedDurations.back());
		ProductionTotalDurations.push_back(
			ProductionShadowDurations[Index]
			+ ProductionGBufferDurations[Index]
			+ ProductionSceneDurations[Index]
			+ ProductionPostProcessDurations[Index]
		);
	}
	std::ranges::sort(ProductionGBufferDurations);
	std::ranges::sort(ProductionSceneDurations);
	std::ranges::sort(ProductionDeferredDurations);
	std::ranges::sort(ProductionDeferredWithoutAODurations);
	std::ranges::sort(ProductionRetainedOpaqueDurations);
	std::ranges::sort(ProductionVolumetricCloudDurations);
	std::ranges::sort(ProductionSortedTranslucencyDurations);
	std::ranges::sort(ProductionRetainedDurations);
	std::ranges::sort(ProductionPostProcessDurations);
	std::ranges::sort(ProductionShadowDurations);
	std::ranges::sort(ProductionTotalDurations);
	std::ranges::sort(FullGTAOFeatureDurations);
	std::ranges::sort(HalfGTAOFeatureDurations);
	std::ranges::sort(HalfGTAOResolveDurations);
	std::ranges::sort(ProductionComputeContactDurations);
	std::ranges::sort(ProductionFragmentContactDurations);
	std::ranges::sort(ConstrainedComputeContactDurations);
	std::ranges::sort(ConstrainedFragmentContactDurations);
	const Durin::uint64 ProductionGBufferMedian = Median(ProductionGBufferDurations);
	const Durin::uint64 ProductionSceneMedian = Median(ProductionSceneDurations);
	const Durin::uint64 ProductionDeferredMedian = Median(ProductionDeferredDurations);
	const Durin::uint64 ProductionDeferredWithoutAOMedian =
		Median(ProductionDeferredWithoutAODurations);
	const Durin::uint64 ProductionAmbientOcclusionCompositionIncrement =
		ProductionDeferredMedian > ProductionDeferredWithoutAOMedian ? ProductionDeferredMedian - ProductionDeferredWithoutAOMedian : 0u;
	const Durin::uint64 ProductionRetainedOpaqueMedian =
		Median(ProductionRetainedOpaqueDurations);
	const Durin::uint64 ProductionVolumetricCloudMedian =
		Median(ProductionVolumetricCloudDurations);
	const Durin::uint64 ProductionSortedTranslucencyMedian =
		Median(ProductionSortedTranslucencyDurations);
	const Durin::uint64 ProductionRetainedMedian = Median(ProductionRetainedDurations);
	const Durin::uint64 ProductionPostProcessMedian = Median(ProductionPostProcessDurations);
	const Durin::uint64 ProductionShadowMedian = Median(ProductionShadowDurations);
	const Durin::uint64 ProductionTotalMedian = Median(ProductionTotalDurations);
	const Durin::uint64 FullGTAOFeatureMedian =
		Median(FullGTAOFeatureDurations);
	const Durin::uint64 HalfGTAOFeatureMedian =
		Median(HalfGTAOFeatureDurations);
	const Durin::uint64 HalfGTAOResolveMedian =
		Median(HalfGTAOResolveDurations);
	const Durin::uint64 ProductionComputeContactMedian =
		Median(ProductionComputeContactDurations);
	const Durin::uint64 ProductionFragmentContactMedian =
		Median(ProductionFragmentContactDurations);
	const Durin::uint64 ConstrainedComputeContactMedian =
		Median(ConstrainedComputeContactDurations);
	const Durin::uint64 ConstrainedFragmentContactMedian =
		Median(ConstrainedFragmentContactDurations);
	const Durin::uint64 ProductionGBufferP95 =
		Percentile95(ProductionGBufferDurations);
	const Durin::uint64 ProductionSceneP95 =
		Percentile95(ProductionSceneDurations);
	const Durin::uint64 ProductionDeferredP95 =
		Percentile95(ProductionDeferredDurations);
	const Durin::uint64 ProductionRetainedOpaqueP95 =
		Percentile95(ProductionRetainedOpaqueDurations);
	const Durin::uint64 ProductionVolumetricCloudP95 =
		Percentile95(ProductionVolumetricCloudDurations);
	const Durin::uint64 ProductionSortedTranslucencyP95 =
		Percentile95(ProductionSortedTranslucencyDurations);
	const Durin::uint64 ProductionRetainedP95 =
		Percentile95(ProductionRetainedDurations);
	const Durin::uint64 ProductionPostProcessP95 =
		Percentile95(ProductionPostProcessDurations);
	const Durin::uint64 ProductionShadowP95 =
		Percentile95(ProductionShadowDurations);
	const Durin::uint64 ProductionTotalP95 =
		Percentile95(ProductionTotalDurations);
	const Durin::uint64 FullGTAOFeatureP95 =
		Percentile95(FullGTAOFeatureDurations);
	const Durin::uint64 HalfGTAOFeatureP95 =
		Percentile95(HalfGTAOFeatureDurations);
	const Durin::uint64 HalfGTAOResolveP95 =
		Percentile95(HalfGTAOResolveDurations);
	const Durin::uint64 ProductionComputeContactP95 =
		Percentile95(ProductionComputeContactDurations);
	const Durin::uint64 ProductionFragmentContactP95 =
		Percentile95(ProductionFragmentContactDurations);
	const Durin::uint64 ConstrainedComputeContactP95 =
		Percentile95(ConstrainedComputeContactDurations);
	const Durin::uint64 ConstrainedFragmentContactP95 =
		Percentile95(ConstrainedFragmentContactDurations);
	EXPECT_GT(ProductionComputeContactMedian, 0u);
	EXPECT_GT(ProductionFragmentContactMedian, 0u);
	EXPECT_LE(ProductionComputeContactMedian,
		ProductionFragmentContactMedian + 300'000u);
	EXPECT_LE(ProductionComputeContactMedian * 100u,
		ProductionFragmentContactMedian * 110u);
	EXPECT_GT(ConstrainedComputeContactMedian, 0u);
	EXPECT_GT(ConstrainedFragmentContactMedian, 0u);
	EXPECT_LE(ConstrainedComputeContactMedian,
		ConstrainedFragmentContactMedian + 300'000u);
	EXPECT_LE(ConstrainedComputeContactMedian * 100u,
		ConstrainedFragmentContactMedian * 110u);
	EXPECT_EQ(ProductionComputeContactCounters.ContactShadowDispatches, 1u);
	EXPECT_EQ(ProductionComputeContactCounters.ContactShadowDraws, 0u);
	EXPECT_EQ(ProductionFragmentContactCounters.ContactShadowDispatches, 0u);
	EXPECT_EQ(ProductionFragmentContactCounters.ContactShadowDraws, 1u);
	EXPECT_EQ(ConstrainedComputeContactCounters.ContactShadowDispatches, 1u);
	EXPECT_EQ(ConstrainedComputeContactCounters.ContactShadowDraws, 0u);
	EXPECT_EQ(ConstrainedFragmentContactCounters.ContactShadowDispatches, 0u);
	EXPECT_EQ(ConstrainedFragmentContactCounters.ContactShadowDraws, 1u);
	// Isolated feature sweeps retain p95 as characterization output only. Their
	// batches run under validation and do not share a frame-level clock or
	// scheduling reference, so cross-batch tails are not regression evidence.
	// The synchronized production route below owns the hard p95 gates.
	EXPECT_GT(FullGTAOFeatureMedian, 0u);
	EXPECT_LE(FullGTAOFeatureMedian, 850'000u);
	EXPECT_GT(HalfGTAOFeatureMedian, 0u);
	EXPECT_GT(HalfGTAOResolveMedian, 0u);
	EXPECT_LE(HalfGTAOFeatureMedian * 100u, FullGTAOFeatureMedian * 65u);
	EXPECT_LE(HalfGTAOFeatureMedian, 600'000u);
	EXPECT_LE(HalfGTAOResolveMedian, 150'000u);
	EXPECT_GT(ProductionGBufferMedian, 0u);
	EXPECT_LE(ProductionGBufferMedian, 350'000u);
	EXPECT_LE(ProductionGBufferP95, 500'000u);
	EXPECT_GT(ProductionDeferredMedian, 0u);
	EXPECT_LE(ProductionDeferredMedian, 450'000u);
	EXPECT_LE(ProductionDeferredP95, 650'000u);
	EXPECT_LE(ProductionAmbientOcclusionCompositionIncrement, 75'000u);
	EXPECT_GT(ProductionRetainedOpaqueMedian, 0u);
	EXPECT_GT(ProductionVolumetricCloudMedian, 0u);
	EXPECT_GT(ProductionSortedTranslucencyMedian, 0u);
	EXPECT_GT(ProductionRetainedMedian, 0u);
	EXPECT_LE(ProductionRetainedMedian, 300'000u);
	EXPECT_LE(ProductionRetainedP95, 500'000u);
	EXPECT_GT(ProductionPostProcessMedian, 0u);
	EXPECT_LE(ProductionPostProcessMedian, 100'000u);
	EXPECT_LE(ProductionPostProcessP95, 120'000u);
	EXPECT_GT(ProductionShadowMedian, 0u);
	EXPECT_GT(ProductionTotalMedian, 0u);
	EXPECT_LE(ProductionTotalMedian, 1'350'000u);
	EXPECT_LE(ProductionTotalP95, 3'000'000u);
	EXPECT_LE(ProductionTotalP95 * 100u, ProductionTotalMedian * 125u)
		<< "Qualification samples are unstable; rerun without competing GPU "
			"work before treating this as a renderer regression.";
	constexpr Durin::uint64 ProductionActiveBytes =
		Durin::FPostProcessRenderer::CalculateSceneTargetBytes(
			TimingWidth, TimingHeight
		)
		+ Durin::FGBufferRenderer::CalculateTargetBytes(TimingWidth, TimingHeight)
		+ Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			TimingWidth, TimingHeight,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution
		)
		+ static_cast<Durin::uint64>(TimingWidth) * TimingHeight * 4u;
	constexpr Durin::uint64 ShadowBytes = 50'331'648u;
	constexpr Durin::uint64 ProductionRetainedCeiling =
		Durin::FPostProcessRenderer::MaximumRetainedSceneTargetBytes
		+ Durin::FGBufferRenderer::MaximumRetainedBytes
		+ Durin::FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes;
	EXPECT_EQ(ProductionActiveBytes, 69'984'000u);
	EXPECT_EQ(ProductionActiveBytes + ShadowBytes, 120'315'648u);
	EXPECT_EQ(ProductionRetainedCeiling, 256u * 1024u * 1024u);
	EXPECT_EQ(ProductionRouteCounters.DeferredDirectionalOutputBytes, 0u);
	EXPECT_EQ(
		ProductionRouteCounters.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(
		ProductionRouteCounters.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(
		ProductionRouteCounters.GroundTruthAmbientOcclusionHalfResolutionViews,
		1u);
	EXPECT_EQ(
		ProductionRouteCounters.GroundTruthAmbientOcclusionUnavailableViews, 0u);
	EXPECT_EQ(ProductionRouteCounters.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			TimingWidth, TimingHeight,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	EXPECT_GE(
		ProductionRouteCounters.GroundTruthAmbientOcclusionRetainedBytes,
		ProductionRouteCounters.GroundTruthAmbientOcclusionActiveBytes);
	EXPECT_LE(
		ProductionRouteCounters.GroundTruthAmbientOcclusionRetainedBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes);
	EXPECT_EQ(ProductionRouteCounters.VolumetricCloudEnabledViews, 0u);
	EXPECT_EQ(ProductionRouteCounters.VolumetricCloudDisabledViews, 1u);
	std::cout << "HYBRID_PRODUCTION_QUALIFICATION"
			  << " gpu=NVIDIA_GeForce_RTX_3090,driver=591.86,vulkan=1.4.325"
			  << ",configuration=Win64-Debug-DurinEditor,validation=enabled"
			  << ",resolution=1920x1080,warmup_frames=" << WarmupFrames
			  << ",measured_frames=" << MeasuredFrames
			  << ",gbuffer_median_ns=" << ProductionGBufferMedian
			  << ",gbuffer_p95_ns=" << ProductionGBufferP95
			  << ",scene_median_ns=" << ProductionSceneMedian
			  << ",scene_p95_ns=" << ProductionSceneP95
			  << ",deferred_median_ns=" << ProductionDeferredMedian
			  << ",deferred_p95_ns=" << ProductionDeferredP95
			  << ",deferred_without_ao_median_ns="
			  << ProductionDeferredWithoutAOMedian
			  << ",ao_composition_increment_median_ns="
			  << ProductionAmbientOcclusionCompositionIncrement
			  << ",retained_opaque_median_ns="
			  << ProductionRetainedOpaqueMedian
			  << ",retained_opaque_p95_ns=" << ProductionRetainedOpaqueP95
			  << ",volumetric_cloud_median_ns="
			  << ProductionVolumetricCloudMedian
			  << ",volumetric_cloud_p95_ns=" << ProductionVolumetricCloudP95
			  << ",volumetric_cloud_enabled_views="
			  << ProductionRouteCounters.VolumetricCloudEnabledViews
			  << ",sorted_translucency_median_ns="
			  << ProductionSortedTranslucencyMedian
			  << ",sorted_translucency_p95_ns="
			  << ProductionSortedTranslucencyP95
			  << ",retained_median_ns=" << ProductionRetainedMedian
			  << ",retained_p95_ns=" << ProductionRetainedP95
			  << ",fxaa_median_ns=" << ProductionPostProcessMedian
			  << ",fxaa_p95_ns=" << ProductionPostProcessP95
			  << ",shadow_median_ns=" << ProductionShadowMedian
			  << ",shadow_p95_ns=" << ProductionShadowP95
			  << ",contact_compute_median_ns="
			  << ProductionComputeContactMedian
			  << ",contact_compute_p95_ns="
			  << ProductionComputeContactP95
			  << ",contact_fragment_median_ns="
			  << ProductionFragmentContactMedian
			  << ",contact_fragment_p95_ns="
			  << ProductionFragmentContactP95
			  << ",contact_constrained_compute_median_ns="
			  << ConstrainedComputeContactMedian
			  << ",contact_constrained_compute_p95_ns="
			  << ConstrainedComputeContactP95
			  << ",contact_constrained_fragment_median_ns="
			  << ConstrainedFragmentContactMedian
			  << ",contact_constrained_fragment_p95_ns="
			  << ConstrainedFragmentContactP95
			  << ",total_median_ns=" << ProductionTotalMedian
			  << ",total_p95_ns=" << ProductionTotalP95
			  << ",full_gtao_feature_median_ns=" << FullGTAOFeatureMedian
			  << ",full_gtao_feature_p95_ns="
			  << FullGTAOFeatureP95
			  << ",half_gtao_feature_median_ns=" << HalfGTAOFeatureMedian
			  << ",half_gtao_feature_p95_ns="
			  << HalfGTAOFeatureP95
			  << ",half_gtao_resolve_median_ns=" << HalfGTAOResolveMedian
			  << ",half_gtao_resolve_p95_ns="
			  << HalfGTAOResolveP95
			  << ",active_bytes=" << ProductionActiveBytes
			  << ",active_with_shadow_bytes=" << ProductionActiveBytes + ShadowBytes
			  << ",retained_ceiling_bytes=" << ProductionRetainedCeiling << '\n';
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "HybridFourFamilyProduction", 320, 180,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 320;
			View.ViewportHeight = 180;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.DirectionalShadow.Candidate =
				Durin::EDirectionalShadowCandidate::SingleMap;
			Durin::FSceneViewRenderOptions Options;
			for (Durin::uint32 Frame = 0; Frame < 2; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastCounters.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.HybridDeferredUnavailableViews, 0u);
	EXPECT_EQ(GLastCounters.GBufferStaticMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			320, 180,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "GroundTruthAmbientOcclusionViewMatrix", 384, 216,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportX = 32;
			View.ViewportY = 18;
			View.ViewportWidth = 320;
			View.ViewportHeight = 180;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.AmbientOcclusion.bEnabled = false;
			Durin::FSceneViewRenderOptions Options;
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			EXPECT_EQ(
				GLastCounters.GroundTruthAmbientOcclusionAttemptedViews, 0u
			);
			EXPECT_EQ(
				GLastCounters.GroundTruthAmbientOcclusionActiveBytes, 0u
			);

			View.Settings.AmbientOcclusion.bEnabled = true;
			for (const auto Mode : {
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Raw,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Confidence,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::Filtered,
					 Durin::EGroundTruthAmbientOcclusionDebugMode::FinalFactor
				 })
			{
				Options.GroundTruthAmbientOcclusionDebugMode = Mode;
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionAttemptedViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionDebugViews, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalDebugViews, 0u);
	EXPECT_EQ(GLastCounters.GroundTruthAmbientOcclusionActiveBytes,
		Durin::FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
			384, 216,
			Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(1), Translate(-0.9, -0.8));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(2), Translate(0.1, -0.9));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(3), Translate(-0.9, 0.1));
	Scene.UpdatePrimitiveTransform(Durin::FPrimitiveSceneId(4), Translate(0.1, 0.1));
	Directional.Intensity = 4.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100), std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FMaterialRenderProxyPublication MutatedPublication;
	MutatedPublication.LocalVersion = 2;
	MutatedPublication.LocalLayer.StaticProperties =
		Durin::FMaterialStaticProperties{
			.BlendMode = Durin::EMaterialBlendMode::Opaque,
			.ShadingModel = Durin::EMaterialShadingModel::Lit,
			.bTwoSided = true
		};
	MutatedPublication.LocalLayer.Parameters.push_back({.Id = Durin::MaterialParameters::BaseColorId, .Type = Durin::EMaterialParameterType::Vector, .VectorValue = {0.2, 0.55, 0.8}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(
		std::move(MutatedPublication)
	));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
								  "HybridFourFamilyMutation", 384, 216,
								  Durin::EPixelFormat::SRGBA8_UNORM
			)
								  .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 384;
			View.ViewportHeight = 216;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			Durin::FSceneViewRenderOptions Options;
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, Options), Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GLastCounters.HybridDeferredEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.GBufferSuccessfulDraws, 4u);
	GBufferQueries.clear();
	DeferredQueries.clear();

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(1));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(3));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(4));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(5));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(6));
	SpecularAAScene.RemovePrimitive(Durin::FPrimitiveSceneId(200));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			StaticQuad->ReleaseResources();
			SpecularAAQuad->ReleaseResources();
			SkeletalQuad->ReleaseResources();
		}
	);
	Durin::FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
