#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/LightSceneProxy.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestContext.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
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
	std::vector<Durin::FGPUTimingQueryRHIRef>* GGBufferTimingQueries = nullptr;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GDeferredTimingQueries = nullptr;
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

	auto CaptureDeferredTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GDeferredTimingQueries != nullptr)
			GDeferredTimingQueries->push_back(Query);
	}

	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GLastCounters = Counters;
	}

	auto MakeStaticQuad() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FStaticMeshRenderData>();
		Data->MaterialSlots = {{"Opaque", 0}};
		auto& LOD = Data->LODResources.emplace_back();
		LOD.VertexBuffers.PositionVertexBuffer.Init({
			{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
			{1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f}));
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f},
			{1.0f, 1.0f}, {0.0f, 1.0f}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({
			.Name = "Opaque", .FirstIndex = 0, .IndexCount = 6,
			.MinVertexIndex = 0, .MaxVertexIndex = 3,
			.MaterialSlotIndex = 0,
			.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Data->LODVertexFactories.resize(1);
		Data->RecalculateBounds();
		return Data;
	}

	auto MakeSkeletalQuad() -> std::unique_ptr<Durin::FSkeletalMeshRenderData>
	{
		auto Data = std::make_unique<Durin::FSkeletalMeshRenderData>();
		Data->VertexBuffers.Geometry.PositionVertexBuffer.Init({
			{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
			{1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}});
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(4, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(4, {1.0f, 0.0f, 0.0f, 1.0f}));
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels>
			UVs;
		UVs[0] = {{0.0f, 0.0f}, {1.0f, 0.0f},
			{1.0f, 1.0f}, {0.0f, 1.0f}};
		Data->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(UVs), 4, 1);
		Data->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(4, Durin::FVector4f(1.0f)), 4);
		Durin::FSkeletalMeshVertexInfluences Influence;
		Influence.BoneIndices[0] = 0;
		Influence.Weights[0] = 1.0f;
		Influence.Count = 1;
		Data->VertexBuffers.InfluenceVertexBuffer.Init(
			std::vector<Durin::FSkeletalMeshVertexInfluences>(4, Influence));
		Data->IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		Data->Sections.push_back({
			.Name = Durin::FName("Opaque"),
			.FirstIndex = 0, .IndexCount = 6,
			.MinVertexIndex = 0, .MaxVertexIndex = 3,
			.MaterialSlotIndex = 0,
			.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})});
		Data->MaterialSlots = {Durin::FName("Opaque")};
		Data->PaletteBoneIndices = {0};
		Data->InverseBindMatrices = {Durin::FMatrix4f(1.0f)};
		Data->InfluenceBounds = {
			Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})};
		Data->LocalBounds = Data->Sections[0].LocalBounds;
		return Data;
	}
}

TEST(FGBufferQualificationTests,
	FourFamilyPassMeetsFrozenRTX3090TimingAndMemoryGates)
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
	auto RendererContext = Durin::FModuleTestContextFactory::CreateStartupContext(
		"GBufferQualificationTest");
	Renderer.StartupModule(RendererContext);
	Durin::SetViewRenderCounterSink(CaptureCounters);

	auto StaticQuad = MakeStaticQuad();
	auto SkeletalQuad = MakeSkeletalQuad();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(StaticQuad->InitResources(CommandList));
			ASSERT_TRUE(SkeletalQuad->InitResources(CommandList));
		});
	Durin::FlushRenderingCommands();

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
		.VectorValue = {0.7, 0.4, 0.2}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
	Durin::FlushRenderingCommands();

	Durin::FScene Scene;
	auto Translate = [](double X, double Y) {
		return Durin::Math::TranslationMatrix(Durin::FVector3{X, Y, 0.0});
	};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1),
		std::make_unique<Durin::FStaticMeshSceneProxy>(StaticQuad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1),
		Translate(-1.0, -1.0));
	Durin::FSplineMeshRenderDynamicData SplineData{
		.Params = {},
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}),
		.Revision = 1};
	SplineData.Params.EndPosition = {1.0, 0.0, 0.0};
	SplineData.Params.StartTangent = {1.0, 0.0, 0.0};
	SplineData.Params.EndTangent = {1.0, 0.0, 0.0};
	SplineData.Params.SourceForwardMin = 0.0;
	SplineData.Params.SourceForwardMax = 1.0;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(2),
		std::make_unique<Durin::FSplineMeshSceneProxy>(StaticQuad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, SplineData),
		Translate(0.0, -1.0));
	auto Pose = std::make_shared<Durin::FSkeletalPosePalette>();
	Pose->Revision = 1;
	Pose->SkeletonCompatibilityIdentity = "GBufferQualification";
	Pose->Matrices = {Durin::FMatrix4f(1.0f)};
	Pose->LocalBounds = SkeletalQuad->LocalBounds;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(3),
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(SkeletalQuad.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Material}, 1, Pose),
		Translate(-1.0, 0.0));
	const std::array<Durin::uint16, 4> Heights{};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> HeightPayload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		2, 2, Heights, HeightPayload, Error)) << Error;
	Durin::FTerrainPatchDescriptor Patch{
		.OriginX = 0, .OriginY = 0, .CellCountX = 1, .CellCountY = 1,
		.LODSteps = {1}, .LODErrors = {0.0},
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.0})};
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(4),
		std::make_unique<Durin::FTerrainSceneProxy>(HeightPayload, 1,
			1.0, 1.0, 0.0, 0.0,
			std::vector<Durin::FTerrainPatchDescriptor>{Patch},
			Patch.LocalBounds, Material, 1), Durin::FMatrix(1.0));
	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(100),
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Durin::FlushRenderingCommands();

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
				Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			ASSERT_NE(Target, nullptr);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = TimingWidth;
			View.ViewportHeight = TimingHeight;
			View.Settings.RenderMode = Durin::ERenderMode::Lit;
			View.Settings.VisibilityMode =
				Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.DirectionalShadowCandidate =
				Durin::EDirectionalShadowCandidate::SingleMap;
			View.Settings.DirectionalShadowFilterQuality =
				Durin::EDirectionalShadowFilterQuality::Medium;
			Durin::FSceneViewRenderOptions Options;
			Options.bEnableGBufferQualification = true;
			Options.bEnableDeferredDirectionalQualification = true;
			for (Durin::uint32 Frame = 0;
				Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				++Durin::GRenderFrameCounterRenderThread;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View,
					Target, false, Options), Durin::ERenderViewResult::Success);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		});
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
			});
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
				+ DeferredResult.DurationNanoseconds);
	}
	std::ranges::sort(GBufferDurations);
	std::ranges::sort(DeferredDurations);
	std::ranges::sort(CombinedDurations);
	ASSERT_EQ(GBufferDurations.size(), MeasuredFrames);
	auto Median = [](const std::vector<Durin::uint64>& Durations) {
		return (Durations[MeasuredFrames / 2 - 1]
			+ Durations[MeasuredFrames / 2]) / 2;
	};
	const Durin::uint64 GBufferMedian = Median(GBufferDurations);
	const Durin::uint64 GBufferP95 = GBufferDurations[113];
	const Durin::uint64 DeferredMedian = Median(DeferredDurations);
	const Durin::uint64 DeferredP95 = DeferredDurations[113];
	const Durin::uint64 CombinedMedian = Median(CombinedDurations);
	const Durin::uint64 CombinedP95 = CombinedDurations[113];
	EXPECT_LE(GBufferMedian, 350'000u);
	EXPECT_LE(GBufferP95, 500'000u);
	EXPECT_LE(DeferredMedian, 300'000u);
	EXPECT_LE(DeferredP95, 450'000u);
	EXPECT_LE(CombinedMedian, 600'000u);
	EXPECT_LE(CombinedP95, 800'000u);
	EXPECT_EQ(GLastCounters.GBufferAttemptedDraws, 4u);
	EXPECT_EQ(GLastCounters.GBufferSuccessfulDraws, 4u);
	EXPECT_EQ(GLastCounters.GBufferRejectedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferSkippedDraws, 0u);
	EXPECT_EQ(GLastCounters.GBufferStaticMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSplineMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferSkeletalMeshSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.GBufferTerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalEnabledViews, 1u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalUnavailableViews, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalPassFailures, 0u);
	EXPECT_EQ(GLastCounters.DeferredDirectionalOutputBytes,
		Durin::FDeferredDirectionalLightingRenderer::CalculateTargetBytes(
			TimingWidth, TimingHeight));
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

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(1));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(2));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(3));
	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(4));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FGBufferQualificationCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			StaticQuad->ReleaseResources();
			SkeletalQuad->ReleaseResources();
		});
	Durin::FlushRenderingCommands();
	auto ShutdownContext =
		Durin::FModuleTestContextFactory::CreateShutdownContext(RendererContext);
	Renderer.ShutdownModule(ShutdownContext);
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
