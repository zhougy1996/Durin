#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/TerrainSceneProxy.h"
#include "GBufferContract.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHICommandList.h"
#include "RHI.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainTopology.h"

#include <gtest/gtest.h>

namespace
{
	Durin::FViewRenderCounters GCounters;
	std::vector<Durin::FViewRenderCounters> GCounterSnapshots;
	std::array<std::vector<Durin::uint8>*, 4> GGBufferPixels{};
	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GCounters = Counters;
		GCounterSnapshots.push_back(Counters);
	}

	auto CaptureGBuffer(
		Durin::FRHICommandListImmediate& CommandList,
		Durin::FRHITexture* Material,
		Durin::FRHITexture* Normals,
		Durin::FRHITexture* Surface,
		Durin::FRHITexture* Emissive,
		Durin::FRHITexture*) -> void
	{
		const std::array Sources{Material, Normals, Surface, Emissive};
		const std::array Names{"TerrainGBufferMaterial", "TerrainGBufferNormals",
			"TerrainGBufferSurface", "TerrainGBufferEmissive"};
		for (size_t Index = 0; Index < Sources.size(); ++Index)
		{
			if (GGBufferPixels[Index] == nullptr) continue;
			Durin::FRHITexture* Source = Sources[Index];
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				Names[Index], Source->GetSizeX(), Source->GetSizeY(),
				Source->GetFormat())
				.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
					| Durin::ETextureCreateFlags::CPUReadback
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Readback =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Readback, nullptr);
			const Durin::FRHITextureSubresourceRange Whole{
				Durin::ERHITextureAspect::Color, 0, 1, 0, 1};
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, Whole,
					Durin::ERHIAccess::GraphicsShaderRead,
					Durin::ERHIAccess::TransferRead},
				Durin::FRHITextureTransition{Readback, Whole,
					Durin::ERHIAccess::Discard,
					Durin::ERHIAccess::TransferWrite}});
			CommandList.CopyTexture(Source, Readback,
				std::array{Durin::FRHITextureCopyRegion{
					.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
			CommandList.TransitionTextures(std::array{
				Durin::FRHITextureTransition{Source, Whole,
					Durin::ERHIAccess::TransferRead,
					Durin::ERHIAccess::GraphicsShaderRead},
				Durin::FRHITextureTransition{Readback, Whole,
					Durin::ERHIAccess::TransferWrite,
					Durin::ERHIAccess::GraphicsShaderRead}});
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Readback, 0, 0, *GGBufferPixels[Index]));
		}
	}

	struct FTerrainRenderCommand
	{
		static constexpr auto GetName() -> const char* { return "TerrainRenderValidation"; }
	};

	auto MakeTerrainTestPerspective() -> Durin::FMatrix
	{
		constexpr double NearClip = 1.0;
		constexpr double FarClip = 20.0;
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = 1.0;
		Projection[2][1] = -1.0;
		Projection[0][2] = FarClip / (FarClip - NearClip);
		Projection[3][2] = -NearClip * FarClip / (FarClip - NearClip);
		Projection[0][3] = 1.0;
		return Projection;
	}
}

TEST(FTerrainRenderVulkanTests, RendersExactHeightPatchAndConservesCounters)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("TerrainRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderCounterSink(CaptureCounters);
	GCounterSnapshots.clear();

	const std::array<Durin::uint16, 9> Samples{
		0, 16384, 32768, 8192, 32768, 49152, 0, 32768, 65535};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(3, 3, Samples, Payload, Error)) << Error;
	auto Material = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
	Durin::FMaterialRenderProxyPublication Publication;
	Publication.LocalVersion = 1;
	Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
		.BlendMode = Durin::EMaterialBlendMode::Opaque,
		.ShadingModel = Durin::EMaterialShadingModel::Lit,
		.bTwoSided = true};
	Publication.LocalLayer.Parameters.push_back({
		.Id = Durin::MaterialParameters::BaseColorId,
		.Type = Durin::EMaterialParameterType::Vector,
		.VectorValue = {0.8f, 0.2f, 0.1f}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
	Durin::FlushRenderingCommands();
	Durin::FTerrainPatchDescriptor Patch{
		.OriginX = 0, .OriginY = 0, .CellCountX = 2, .CellCountY = 2,
		.LODSteps = {1, 2}, .LODErrors = {0.0, 0.0},
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.5})};
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(Payload, 1, 0.5, 0.5,
			0.5, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch},
			Patch.LocalBounds, Material, 1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();

	auto Readback = std::make_shared<std::vector<Durin::uint8>>();
	auto QualificationReadback =
		std::make_shared<std::vector<Durin::uint8>>();
	std::array<std::vector<Durin::uint8>, 4> TerrainGBufferPixels;
	for (size_t Index = 0; Index < GGBufferPixels.size(); ++Index)
		GGBufferPixels[Index] = &TerrainGBufferPixels[Index];
	Durin::SetGBufferCaptureSink(CaptureGBuffer);
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene, Readback, QualificationReadback](
			Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainValidationColor", 65, 65, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 65;
			View.ViewportHeight = 65;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = Durin::EViewLODMode::ForceLOD0;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Target, 0, 0, *Readback));
			Durin::FSceneViewRenderOptions QualificationOptions;
			QualificationOptions.bEnableGBufferQualification = true;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target,
				false, QualificationOptions), Durin::ERenderViewResult::Success);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *QualificationReadback));
			View.Settings.LODMode = Durin::EViewLODMode::Automatic;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	Durin::SetGBufferCaptureSink(nullptr);
	GGBufferPixels.fill(nullptr);
	EXPECT_EQ(Readback->size(), 65u * 65u * 4u);
	EXPECT_TRUE(std::ranges::any_of(*Readback, [](Durin::uint8 Value) { return Value != 0; }));
	EXPECT_EQ(*QualificationReadback, *Readback);
	ASSERT_EQ(GCounterSnapshots.size(), 3u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferEnabledViews, 1u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferAttemptedDraws, 1u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferSuccessfulDraws, 1u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferRejectedDraws, 0u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferSkippedDraws, 0u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferTerrainAttemptedDraws, 1u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferTerrainRejectedDraws, 0u);
	EXPECT_EQ(GCounterSnapshots[1].GBufferTerrainSkippedDraws, 0u);
	for (const auto& Attachment : TerrainGBufferPixels)
		ASSERT_EQ(Attachment.size(), 65u * 65u * 4u);
	size_t ValidTerrainGBufferPixels = 0;
	for (size_t Offset = 0; Offset < TerrainGBufferPixels[2].size(); Offset += 4)
	{
		if (TerrainGBufferPixels[2][Offset + 3] == 0u) continue;
		++ValidTerrainGBufferPixels;
		EXPECT_EQ(TerrainGBufferPixels[2][Offset + 3],
			Durin::GBufferContract::StandardLitFlag);
		EXPECT_NEAR(static_cast<float>(TerrainGBufferPixels[0][Offset]) / 255.0f,
			0.8f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(static_cast<float>(TerrainGBufferPixels[0][Offset + 1]) / 255.0f,
			0.2f, Durin::GBufferContract::MaximumUNorm8Error);
		EXPECT_NEAR(static_cast<float>(TerrainGBufferPixels[0][Offset + 2]) / 255.0f,
			0.1f, Durin::GBufferContract::MaximumUNorm8Error);
		const Durin::FVector3f ShadingNormal =
			Durin::GBufferContract::DecodeOctahedralNormal({
				static_cast<float>(TerrainGBufferPixels[1][Offset]) / 255.0f,
				static_cast<float>(TerrainGBufferPixels[1][Offset + 1]) / 255.0f});
		EXPECT_NEAR(Durin::Math::Length(ShadingNormal), 1.0, 1.0e-5);
	}
	EXPECT_GT(ValidTerrainGBufferPixels, 0u);
	EXPECT_EQ(GCounterSnapshots[0].PreparedTerrainTriangles, 8u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainHeightUploadBytes, 18u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainHeightUploads, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainTopologyCreations, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainShaderLookups, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainShaderCreations, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainShaderReuses, 0u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainPipelineLookups, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainPipelineCreations, 1u);
	EXPECT_EQ(GCounterSnapshots[0].TerrainPipelineReuses, 0u);
	EXPECT_GT(GCounterSnapshots[0].TerrainHeightPreparationNanoseconds, 0u);
	EXPECT_GT(GCounterSnapshots[0].TerrainTopologyPreparationNanoseconds, 0u);
	EXPECT_GT(GCounterSnapshots[0].TerrainShaderPreparationNanoseconds, 0u);
	EXPECT_GT(GCounterSnapshots[0].TerrainPipelinePreparationNanoseconds, 0u);
	EXPECT_EQ(GCounterSnapshots[0].RequestedTerrainLODHistogram,
		(std::vector<size_t>{1u}));
	EXPECT_EQ(GCounters.VisibleTerrainCandidates, 1u);
	EXPECT_EQ(GCounters.TerrainPatchCandidates, 1u);
	EXPECT_EQ(GCounters.VisibleTerrainPatches, 1u);
	EXPECT_EQ(GCounters.PreparedTerrainTriangles, 2u);
	EXPECT_EQ(GCounters.TerrainHeightReuses, 1u);
	EXPECT_EQ(GCounters.RequestedTerrainLODHistogram,
		(std::vector<size_t>{0u, 1u}));
	EXPECT_EQ(GCounters.ResolvedTerrainLODHistogram,
		(std::vector<size_t>{0u, 1u}));
	EXPECT_EQ(GCounters.TerrainAttemptedDraws, 1u);
	EXPECT_EQ(GCounters.TerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GCounters.TerrainRejectedDraws, 0u);
	EXPECT_EQ(GCounters.TerrainShaderLookups,
		GCounters.TerrainShaderCreations + GCounters.TerrainShaderReuses);
	EXPECT_EQ(GCounters.TerrainPipelineLookups,
		GCounters.TerrainPipelineCreations + GCounters.TerrainPipelineReuses);

	// Closing and reopening the same immutable generation in one renderer lifetime
	// must not repeat height, topology, shader, or pipeline creation.
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(91));
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(Payload, 1, 0.5, 0.5,
			0.5, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch},
			Patch.LocalBounds, Material, 1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainReopenColor", 65, 65, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 65;
			View.ViewportHeight = 65;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = Durin::EViewLODMode::ForceLOD0;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GCounters.TerrainHeightUploads, 0u);
	EXPECT_EQ(GCounters.TerrainHeightReuses, 1u);
	EXPECT_EQ(GCounters.TerrainTopologyCreations, 0u);
	EXPECT_EQ(GCounters.TerrainTopologyReuses, 1u);
	EXPECT_EQ(GCounters.TerrainShaderCreations, 0u);
	EXPECT_EQ(GCounters.TerrainShaderReuses, 1u);
	EXPECT_EQ(GCounters.TerrainPipelineCreations, 0u);
	EXPECT_EQ(GCounters.TerrainPipelineReuses, 1u);

	// Device invalidation deliberately drops every dependent retained resource;
	// the next draw reconstructs a complete set on demand.
	ASSERT_TRUE(Durin::RequestRendererDeviceInvalidation().bSuccess);
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainDeviceRecoveryColor", 65, 65,
				Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 65;
			View.ViewportHeight = 65;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = Durin::EViewLODMode::ForceLOD0;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GCounters.TerrainHeightUploads, 1u);
	EXPECT_EQ(GCounters.TerrainTopologyCreations, 1u);
	EXPECT_EQ(GCounters.TerrainShaderCreations, 1u);
	EXPECT_EQ(GCounters.TerrainPipelineCreations, 1u);
	EXPECT_EQ(GCounters.TerrainSuccessfulDraws, 1u);

	const Durin::FVector3 LargeWorldOrigin{10000000.25, -10000000.5, 0.0};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(Payload, 1, 0.5, 0.5,
			0.5, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch},
			Patch.LocalBounds, Material, 1),
		Durin::Math::TranslationMatrix(LargeWorldOrigin));
	Durin::FlushRenderingCommands();
	auto LargeCoordinateReadback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene, LargeCoordinateReadback, LargeWorldOrigin](
			Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainLargeCoordinate", 65, 65, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewMatrix = Durin::Math::TranslationMatrix(-LargeWorldOrigin);
			View.ViewProjectionMatrix = View.ViewMatrix;
			View.ViewLocation = LargeWorldOrigin;
			View.ViewportWidth = 65;
			View.ViewportHeight = 65;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = Durin::EViewLODMode::ForceLOD0;
			Durin::FSceneViewRenderOptions QualificationOptions;
			QualificationOptions.bEnableGBufferQualification = true;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false,
				QualificationOptions),
				Durin::ERenderViewResult::Success);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Target, 0, 0, *LargeCoordinateReadback));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(*LargeCoordinateReadback, *Readback);
	EXPECT_EQ(GCounters.GBufferTerrainAttemptedDraws, 1u);
	EXPECT_EQ(GCounters.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GCounters.GBufferTerrainRejectedDraws, 0u);

	const std::array<Durin::uint16, 15> MixedSamples{
		65535, 65535, 65535, 65535, 65535,
		65535, 65535, 65535, 65535, 65535,
		65535, 65535, 65535, 65535, 65535};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> MixedPayload;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		5, 3, MixedSamples, MixedPayload, Error)) << Error;
	Durin::FTerrainPatchDescriptor West;
	West.OriginX = 0; West.GridX = 0;
	West.CellCountX = 2; West.CellCountY = 2;
	West.LODSteps = {1, 2}; West.LODErrors = {0.0, 0.0};
	West.LocalBounds = Durin::FBox({0.0, 0.0, 0.5}, {0.5, 1.0, 0.5});
	Durin::FTerrainPatchDescriptor East = West;
	East.OriginX = 2; East.GridX = 1;
	East.LODErrors = {0.0, 0.3};
	East.LocalBounds = Durin::FBox({0.5, 0.0, 0.5}, {1.0, 1.0, 0.5});
	Durin::FMatrix MixedTransform(1.0);
	MixedTransform[3][0] = 5.0;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(MixedPayload, 2, 0.25, 0.5,
			0.5, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{West, East},
			Durin::FBox({0.0, 0.0, 0.5}, {1.0, 1.0, 0.5}), Material, 1),
		MixedTransform);
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {-1.0, 0.0, -1.0};
	Directional.Intensity = 1.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(10),
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainMixedLODColor", 65, 65, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ProjectionMatrix = MakeTerrainTestPerspective();
			View.ViewProjectionMatrix = View.ProjectionMatrix;
			View.ViewportWidth = 65;
			View.ViewportHeight = 65;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.bShowTerrainLODOverlay = true;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GCounters.PreparedTerrainTriangles, 9u);
	EXPECT_EQ(GCounters.RequestedTerrainLODHistogram,
		(std::vector<size_t>{1u, 1u}));
	EXPECT_EQ(GCounters.ResolvedTerrainLODHistogram,
		(std::vector<size_t>{1u, 1u}));
	EXPECT_EQ(GCounters.TerrainStitchMaskHistogram[0], 1u);
	EXPECT_EQ(GCounters.TerrainStitchMaskHistogram[
		static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::West)], 1u);
	EXPECT_EQ(GCounters.TerrainAdjacencyPromotions, 0u);
	// The preceding device invalidation cleared both topology keys; this mixed
	// view rebuilds each exact key once.
	EXPECT_EQ(GCounters.TerrainTopologyCreations, 2u);
	EXPECT_EQ(GCounters.TerrainTopologyReuses, 0u);
	EXPECT_EQ(GCounters.TerrainSuccessfulDraws, 2u);
	EXPECT_EQ(GCounters.ShadowPreparedTerrainCasters, 6u);
	EXPECT_EQ(GCounters.ShadowSuccessfulDraws, 6u);

	Scene.RemoveLight(Durin::FLightSceneId(10));
	std::vector<Durin::uint16> MaskSamples(7u * 7u, 65535);
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> MaskPayload;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		7, 7, MaskSamples, MaskPayload, Error)) << Error;
	for (Durin::uint8 Mask = 0; Mask < 16; ++Mask)
	{
		std::vector<Durin::FTerrainPatchDescriptor> MaskPatches;
		for (Durin::uint16 GridY = 0; GridY < 3; ++GridY)
			for (Durin::uint16 GridX = 0; GridX < 3; ++GridX)
			{
				Durin::FTerrainPatchDescriptor MaskPatch;
				MaskPatch.OriginX = GridX * 2;
				MaskPatch.OriginY = GridY * 2;
				MaskPatch.GridX = GridX;
				MaskPatch.GridY = GridY;
				MaskPatch.CellCountX = 2;
				MaskPatch.CellCountY = 2;
				MaskPatch.LODSteps = {1, 2};
				const bool Center = GridX == 1 && GridY == 1;
				const bool CoarseNorth = GridX == 1 && GridY == 0
					&& (Mask & static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::North));
				const bool CoarseEast = GridX == 2 && GridY == 1
					&& (Mask & static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::East));
				const bool CoarseSouth = GridX == 1 && GridY == 2
					&& (Mask & static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::South));
				const bool CoarseWest = GridX == 0 && GridY == 1
					&& (Mask & static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::West));
				const bool bCoarse = !Center
					&& (CoarseNorth || CoarseEast || CoarseSouth || CoarseWest);
				MaskPatch.LODErrors = bCoarse ? std::vector<double>{0.0, 0.0}
					: std::vector<double>{0.0, 0.5};
				MaskPatch.LocalBounds = Durin::FBox(
					{GridX / 3.0, GridY / 3.0, 0.5},
					{(GridX + 1) / 3.0, (GridY + 1) / 3.0, 0.5});
				MaskPatches.push_back(std::move(MaskPatch));
			}
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
			std::make_unique<Durin::FTerrainSceneProxy>(MaskPayload, 10 + Mask,
				1.0 / 6.0, 1.0 / 6.0, 0.5, 0.0, std::move(MaskPatches),
				Durin::FBox({0.0, 0.0, 0.5}, {1.0, 1.0, 0.5}), Material, 1),
			MixedTransform);
		Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
			[&Renderer, &Scene, Mask](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					"TerrainAllStitchMasks", 33, 33, Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				Durin::FSceneView View;
				View.ProjectionMatrix = MakeTerrainTestPerspective();
				View.ViewProjectionMatrix = View.ProjectionMatrix;
				View.ViewportWidth = 33;
				View.ViewportHeight = 33;
				View.Settings.RenderMode = Durin::ERenderMode::Unlit;
				View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.bDisableTerrainBatching = Mask == 0;
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
					Durin::ERenderViewResult::Success);
				EXPECT_GE(GCounters.TerrainStitchMaskHistogram[Mask], 1u);
				EXPECT_EQ(GCounters.TerrainSuccessfulDraws,
					GCounters.PreparedTerrainBatches);
				EXPECT_EQ(GCounters.TerrainSubmittedLogicalPatches,
					GCounters.VisibleTerrainPatches);
				if (Mask == 0)
					EXPECT_EQ(GCounters.PreparedTerrainBatches,
						GCounters.VisibleTerrainPatches);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
	}
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"TerrainRadialDistance", 33, 33, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 33;
			View.ViewportHeight = 33;
			View.DepthConvention = Durin::ESceneDepthConvention::ReversedZ;
			View.FarClipDistance = 20000.0;
			View.ViewFadeStart = 4.0;
			View.ViewRenderDistance = 6.0;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			const size_t SelectedAtFirstYaw = GCounters.VisibleTerrainPatches;
			EXPECT_EQ(GCounters.RadialRejectedTerrainPatches, 0u);
			EXPECT_GT(GCounters.TransitionTerrainPatches, 0u);
			View.ViewMatrix = Durin::Math::RotationMatrix(
				Durin::Math::MakeQuaternionFromAxisAngleRadians(
					Durin::Math::Pi<double>(), Durin::FVectorConstants::Up));
			View.ViewProjectionMatrix = View.ViewMatrix;
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			EXPECT_EQ(GCounters.VisibleTerrainPatches, SelectedAtFirstYaw);
			View.ViewLocation = {-10.0, 0.0, 0.0};
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			EXPECT_EQ(GCounters.VisibleTerrainPatches, 0u);
			EXPECT_EQ(GCounters.RadialRejectedTerrainPatches,
				GCounters.TerrainPatchCandidates);
			EXPECT_EQ(GCounters.TerrainResourceAttemptedDraws, 0u);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(91));
	Durin::FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
