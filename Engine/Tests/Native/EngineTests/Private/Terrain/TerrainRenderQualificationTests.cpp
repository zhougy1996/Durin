#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHICommandList.h"
#include "RHI.h"
#include "RendererModule.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/SceneRendererProfiling.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Terrain/TerrainHeightmap.h"

#include <gtest/gtest.h>

namespace
{
	Durin::FViewRenderTelemetry GTelemetry;
	std::vector<Durin::FViewRenderTelemetry> GTelemetrySnapshots;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GTimingQueries = nullptr;

	auto CaptureTelemetry(const Durin::FViewRenderTelemetry& Telemetry) -> void
	{
		GTelemetry = Telemetry;
		GTelemetrySnapshots.push_back(Telemetry);
	}

	auto CaptureTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GTimingQueries) GTimingQueries->push_back(Query);
	}

	struct FTerrainQualificationCommand
	{
		static constexpr auto GetName() -> const char* { return "TerrainRenderQualification"; }
	};
}

TEST(FTerrainRenderQualificationTests, MeasuresMaximumHeightPatchRendering)
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
	Durin::FModuleTestHarness RendererLifecycle("TerrainQualificationRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::SetViewRenderTelemetrySink(CaptureTelemetry);

	constexpr uint32 MaximumSamples = 1025;
	std::vector<uint16> MaximumPlane(
		static_cast<size_t>(MaximumSamples) * MaximumSamples, 32768);
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> MaximumPayload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		MaximumSamples, MaximumSamples, MaximumPlane, MaximumPayload, Error)) << Error;

	auto Material = Durin::MakeRefCount<Durin::FMaterialRenderProxy>();
	Durin::FMaterialRenderProxyPublication Publication;
	Publication.LocalVersion = 1;
	Publication.LocalLayer.StaticProperties = Durin::FMaterialStaticProperties{
		.BlendMode = Durin::EMaterialBlendMode::Opaque,
		.ShadingModel = Durin::EMaterialShadingModel::Unlit,
		.bTwoSided = true};
	Publication.LocalLayer.Parameters.push_back({
		.Id = Durin::MaterialParameters::BaseColorId,
		.Type = Durin::EMaterialParameterType::Vector,
		.VectorValue = {0.8f, 0.2f, 0.1f}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
	Durin::FlushRenderingCommands();

	std::vector<Durin::FTerrainPatchDescriptor> MaximumPatches;
	MaximumPatches.reserve(256);
	for (uint32 Y = 0; Y < 1024; Y += 64)
		for (uint32 X = 0; X < 1024; X += 64)
		{
			Durin::FTerrainPatchDescriptor Patch;
			Patch.OriginX = X;
			Patch.OriginY = Y;
			Patch.GridX = static_cast<uint16>(X / 64);
			Patch.GridY = static_cast<uint16>(Y / 64);
			Patch.CellCountX = 64;
			Patch.CellCountY = 64;
			Patch.LODSteps = {1, 2, 4, 8, 16, 32, 64};
			Patch.LODErrors.assign(Patch.LODSteps.size(), 0.0);
			Patch.LocalBounds = Durin::FBox({X / 1024.0, Y / 1024.0, 0.25},
				{(X + 64) / 1024.0, (Y + 64) / 1024.0, 0.25});
			MaximumPatches.push_back(std::move(Patch));
		}
	Durin::FScene Scene;
	auto MaximumProxy = std::make_unique<Durin::FTerrainSceneProxy>(MaximumPayload, 2,
			1.0 / 1024.0, 1.0 / 1024.0, 0.5, 0.0,
			std::move(MaximumPatches), Durin::FBox({0.0, 0.0, 0.25}, {1.0, 1.0, 0.25}),
			Material, 1);
	EXPECT_LE(MaximumProxy->GetLODMetadataBytes(), 64u * 1024u);
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::move(MaximumProxy), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();

	constexpr uint32 WarmupFrames = 2;
	constexpr uint32 MeasuredFrames = 7;
	std::vector<Durin::FGPUTimingQueryRHIRef> TimingQueries;
	std::vector<double> CpuMilliseconds;
	double FirstFrameCpuMilliseconds = 0.0;
	GTelemetrySnapshots.clear();
	GTimingQueries = &TimingQueries;
	Durin::SetSceneColorTimingQuerySink(CaptureTiming);
	Durin::EnqueueRenderCommand<FTerrainQualificationCommand>(
		[&Renderer, &Scene, &CpuMilliseconds, &FirstFrameCpuMilliseconds](
			Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"MaximumTerrainQualificationColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 17;
			View.ViewportHeight = 17;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.Mode.LODMode = Durin::EViewLODMode::ForceLOD0;
			for (uint32 Frame = 0; Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Begin = std::chrono::steady_clock::now();
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
					Durin::ERenderViewResult::Success);
				const auto End = std::chrono::steady_clock::now();
				if (Frame == 0)
					FirstFrameCpuMilliseconds = std::chrono::duration<double, std::milli>(
						End - Begin).count();
				if (Frame >= WarmupFrames)
					CpuMilliseconds.push_back(
						std::chrono::duration<double, std::milli>(End - Begin).count());
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		});
	Durin::FlushRenderingCommands();
	Durin::SetSceneColorTimingQuerySink(nullptr);
	GTimingQueries = nullptr;

	for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		const bool Ready = TimingQueries.size() == WarmupFrames + MeasuredFrames
			&& std::ranges::all_of(TimingQueries, [](const auto& Query) {
				return Query->GetResult().State == Durin::ERHIGPUTimingResultState::Ready;
			});
		if (Ready) break;
		Durin::EnqueueRenderCommand<FTerrainQualificationCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}

	EXPECT_EQ(GTelemetry.Terrain.TerrainPatchCandidates, 256u);
	EXPECT_EQ(GTelemetry.Terrain.VisibleTerrainPatches, 256u);
	EXPECT_EQ(GTelemetry.Terrain.PreparedTerrainTriangles, 2'097'152u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainHeightUploadBytes, 0u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainHeightReuses, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainTopologyCreations, 0u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainTopologyReuses, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainShaderCreations, 0u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainShaderReuses, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainPipelineCreations, 0u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainPipelineReuses, 1u);
	EXPECT_EQ(GTelemetry.Terrain.PreparedTerrainBatches, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainBatchChunks, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainInstances, 256u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainInstanceBytes,
		256u * Durin::TerrainInstanceDataBytes);
	EXPECT_EQ(GTelemetry.Terrain.TerrainInstanceAllocations, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainResourceAttemptedBatches, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainResourceSuccessfulBatches, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainSubmittedLogicalPatches, 256u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainAttemptedDraws, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainSuccessfulDraws, 1u);
	ASSERT_EQ(GTelemetrySnapshots.size(), WarmupFrames + MeasuredFrames);
	const Durin::FViewRenderTelemetry& FirstFrame = GTelemetrySnapshots.front();
	EXPECT_EQ(FirstFrame.Terrain.TerrainHeightUploads, 1u);
	EXPECT_EQ(FirstFrame.Terrain.TerrainHeightUploadBytes,
		static_cast<size_t>(MaximumSamples) * MaximumSamples * sizeof(uint16));
	EXPECT_EQ(FirstFrame.Terrain.TerrainTopologyCreations, 1u);
	EXPECT_EQ(FirstFrame.Terrain.TerrainShaderLookups,
		FirstFrame.Terrain.TerrainShaderCreations + FirstFrame.Terrain.TerrainShaderReuses);
	EXPECT_EQ(FirstFrame.Terrain.TerrainPipelineLookups,
		FirstFrame.Terrain.TerrainPipelineCreations + FirstFrame.Terrain.TerrainPipelineReuses);
	EXPECT_EQ(FirstFrame.Terrain.TerrainShaderCreations, 1u);
	EXPECT_EQ(FirstFrame.Terrain.TerrainPipelineCreations, 1u);
	EXPECT_GT(FirstFrame.Terrain.TerrainHeightPreparationNanoseconds, 0u);
	EXPECT_GT(FirstFrame.Terrain.TerrainTopologyPreparationNanoseconds, 0u);
	EXPECT_GT(FirstFrame.Terrain.TerrainShaderPreparationNanoseconds, 0u);
	EXPECT_GT(FirstFrame.Terrain.TerrainPipelinePreparationNanoseconds, 0u);
	EXPECT_LE(FirstFrame.Terrain.TerrainHeightPreparationNanoseconds
			+ FirstFrame.Terrain.TerrainTopologyPreparationNanoseconds
			+ FirstFrame.Terrain.TerrainShaderPreparationNanoseconds
			+ FirstFrame.Terrain.TerrainPipelinePreparationNanoseconds,
		FirstFrame.Terrain.TerrainResourcePreparationNanoseconds);
	EXPECT_LE(FirstFrameCpuMilliseconds, 5000.0);
	EXPECT_GT(GTelemetry.Terrain.TerrainLogicalPreparationNanoseconds, 0u);
	EXPECT_GT(GTelemetry.Terrain.TerrainBatchConstructionNanoseconds, 0u);
	EXPECT_GT(GTelemetry.Terrain.TerrainResourcePreparationNanoseconds, 0u);
	EXPECT_GT(GTelemetry.Terrain.TerrainDynamicAllocationNanoseconds, 0u);
	EXPECT_GT(GTelemetry.Terrain.TerrainCommandRecordingNanoseconds, 0u);
	EXPECT_EQ(CpuMilliseconds.size(), MeasuredFrames);
	EXPECT_EQ(TimingQueries.size(), WarmupFrames + MeasuredFrames);
	std::ranges::sort(CpuMilliseconds);
	std::vector<double> GpuMilliseconds;
	for (size_t Index = WarmupFrames; Index < TimingQueries.size(); ++Index)
	{
		const auto Result = TimingQueries[Index]->GetResult();
		EXPECT_EQ(Result.State, Durin::ERHIGPUTimingResultState::Ready);
		if (Result.State == Durin::ERHIGPUTimingResultState::Ready)
			GpuMilliseconds.push_back(Result.DurationNanoseconds / 1'000'000.0);
	}
	std::ranges::sort(GpuMilliseconds);
	if (!CpuMilliseconds.empty())
	{
		EXPECT_LE(CpuMilliseconds[CpuMilliseconds.size() / 2], 150.0);
		EXPECT_LE(CpuMilliseconds.back(), 250.0);
	}
	if (!GpuMilliseconds.empty()) EXPECT_LE(GpuMilliseconds.back(), 50.0);
	if (!CpuMilliseconds.empty() && !GpuMilliseconds.empty())
		std::cout << "[ TERRAIN ] 1025x1025: cpu median="
			<< CpuMilliseconds[CpuMilliseconds.size() / 2] << "ms p95="
			<< CpuMilliseconds.back() << "ms; gpu median="
			<< GpuMilliseconds[GpuMilliseconds.size() / 2] << "ms p95="
			<< GpuMilliseconds.back() << "ms\n";
	std::cout << "[ TERRAIN ] 1025x1025 first frame: cpu="
		<< FirstFrameCpuMilliseconds << "ms; height="
		<< FirstFrame.Terrain.TerrainHeightPreparationNanoseconds / 1'000'000.0
		<< "ms topology="
		<< FirstFrame.Terrain.TerrainTopologyPreparationNanoseconds / 1'000'000.0
		<< "ms shader="
		<< FirstFrame.Terrain.TerrainShaderPreparationNanoseconds / 1'000'000.0
		<< "ms pipeline="
		<< FirstFrame.Terrain.TerrainPipelinePreparationNanoseconds / 1'000'000.0
		<< "ms command="
		<< FirstFrame.Terrain.TerrainCommandRecordingNanoseconds / 1'000'000.0
		<< "ms\n";

	double AutomaticCpuMilliseconds = 0.0;
	Durin::EnqueueRenderCommand<FTerrainQualificationCommand>(
		[&Renderer, &Scene, &AutomaticCpuMilliseconds](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"MaximumTerrainAutomaticColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 17;
			View.ViewportHeight = 17;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			const auto Begin = std::chrono::steady_clock::now();
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			AutomaticCpuMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - Begin).count();
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(GTelemetry.Terrain.PreparedTerrainTriangles, 512u);
	ASSERT_EQ(GTelemetry.Terrain.RequestedTerrainLODHistogram.size(), 7u);
	EXPECT_EQ(GTelemetry.Terrain.RequestedTerrainLODHistogram[6], 256u);
	EXPECT_EQ(GTelemetry.Terrain.ResolvedTerrainLODHistogram[6], 256u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainLODFallbacks, 0u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainLODResolutionFallbacks, 0u);
	EXPECT_EQ(GTelemetry.Terrain.PreparedTerrainBatches, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainInstances, 256u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GTelemetry.Terrain.TerrainSubmittedLogicalPatches, 256u);
	EXPECT_LE(AutomaticCpuMilliseconds, 150.0);
	std::cout << "[ TERRAIN ] 1025x1025 automatic flat: cpu="
		<< AutomaticCpuMilliseconds << "ms; triangles="
		<< GTelemetry.Terrain.PreparedTerrainTriangles << "\n";

	Durin::FDirectionalLightSceneData Directional;
	Directional.Direction = {0.35, 0.2, -1.0};
	Directional.Color = {1.0f, 1.0f, 1.0f};
	Directional.Intensity = 3.0f;
	Directional.bCastShadows = true;
	Scene.AddOrReplaceLight(
		Durin::FLightSceneId(100),
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Directional));
	Scene.UpdatePrimitiveTransform(
		Durin::FPrimitiveSceneId(91),
		Durin::Math::TranslationMatrix(Durin::FVector3(24.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();
	constexpr size_t ShadowWarmupFrames = 30u;
	constexpr size_t ShadowMeasuredFrames = 120u;
	struct FShadowProfile
	{
		std::vector<uint64> Logical;
		std::vector<uint64> Terrain;
		Durin::FViewRenderTelemetry Telemetry;
	};
	auto ProfileShadowCandidate = [&Renderer, &Scene](
		Durin::EDirectionalShadowCandidate Candidate,
		const char* TargetName) {
		auto Profile = std::make_shared<FShadowProfile>();
		Durin::EnqueueRenderCommand<FTerrainQualificationCommand>(
			[&Renderer, &Scene, Candidate, TargetName, Profile](
				Durin::FRHICommandListImmediate& CommandList) {
				const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
					TargetName, 1920, 1080,
					Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource);
				Durin::FTextureRHIRef Target =
					Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Target, nullptr);
				Durin::FSceneView View;
				constexpr double NearClip = 1.0;
				constexpr double FarClip = 300.0;
				const double YScale = 1.0 / std::tan(
					Durin::Math::DegreesToRadians(60.0) * 0.5);
				View.ProjectionMatrix = Durin::FMatrix(0.0);
				View.ProjectionMatrix[1][0] = YScale;
				View.ProjectionMatrix[2][1] = -YScale;
				View.ProjectionMatrix[0][2] =
					FarClip / (FarClip - NearClip);
				View.ProjectionMatrix[3][2] =
					-NearClip * FarClip / (FarClip - NearClip);
				View.ProjectionMatrix[0][3] = 1.0;
				View.ViewProjectionMatrix = View.ProjectionMatrix;
				View.ViewportWidth = 1920;
				View.ViewportHeight = 1080;
				View.Settings.Mode.RenderMode = Durin::ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					Durin::EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.Candidate = Candidate;
				View.Settings.DirectionalShadow.FilterQuality =
					Durin::EDirectionalShadowFilterQuality::Medium;
				View.Settings.DirectionalShadow.bEnableContactShadows = false;
				for (size_t Frame = 0;
					 Frame < ShadowWarmupFrames + ShadowMeasuredFrames; ++Frame)
				{
					++Durin::GRenderFrameCounterRenderThread;
					Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					ASSERT_EQ(Renderer.RenderView(
						CommandList, &Scene, View, Target, false, {}),
						Durin::ERenderViewResult::Success);
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					if (Frame >= ShadowWarmupFrames)
					{
						Profile->Logical.push_back(
							GTelemetry.DirectionalShadow.ShadowLogicalPreparationNanoseconds);
						Profile->Terrain.push_back(
							GTelemetry.DirectionalShadow.ShadowTerrainLogicalPreparationNanoseconds);
					}
				}
				Profile->Telemetry = GTelemetry;
			});
		Durin::FlushRenderingCommands();
		return *Profile;
	};
	const FShadowProfile SingleMap = ProfileShadowCandidate(
		Durin::EDirectionalShadowCandidate::SingleMap,
		"MaximumTerrainSingleShadowQualification");
	const FShadowProfile ThreeCascades = ProfileShadowCandidate(
		Durin::EDirectionalShadowCandidate::ThreeCascades,
		"MaximumTerrainCascadeShadowQualification");
	auto Summarize = [](std::vector<uint64> Values) {
		std::ranges::sort(Values);
		const size_t P95 = std::min(
			Values.size() - 1u,
			static_cast<size_t>(std::ceil(
				static_cast<double>(Values.size()) * 0.95)) - 1u);
		return std::pair{Values[Values.size() / 2u], Values[P95]};
	};
	ASSERT_EQ(SingleMap.Logical.size(), ShadowMeasuredFrames);
	ASSERT_EQ(ThreeCascades.Logical.size(), ShadowMeasuredFrames);
	const auto SingleLogical = Summarize(SingleMap.Logical);
	const auto CascadeLogical = Summarize(ThreeCascades.Logical);
	const auto CascadeTerrain = Summarize(ThreeCascades.Terrain);
	EXPECT_EQ(ThreeCascades.Telemetry.DirectionalShadow.ShadowSceneTraversals, 1u);
	EXPECT_EQ(
		ThreeCascades.Telemetry.DirectionalShadow.ShadowUniqueEligibleTerrainCasters, 1u);
	EXPECT_EQ(
		ThreeCascades.Telemetry.DirectionalShadow.ShadowCascadeClassificationTests,
		Durin::DirectionalShadowCascadeCount);
	EXPECT_GE(ThreeCascades.Telemetry.DirectionalShadow.ShadowMembershipPopcount, 2u);
	EXPECT_EQ(
		ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPrimitiveFactBuilds,
		ThreeCascades.Telemetry.DirectionalShadow.ShadowMembershipPopcount);
	EXPECT_EQ(ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPrimitiveFactReuses, 0u);
	EXPECT_EQ(
		ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPatchFactBuilds,
		ThreeCascades.Telemetry.DirectionalShadow.ShadowMembershipPopcount * 256u);
	EXPECT_EQ(ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPatchFactReuses, 0u);
	EXPECT_EQ(
		ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPatchClassificationTests,
		ThreeCascades.Telemetry.DirectionalShadow.ShadowMembershipPopcount * 256u);
	EXPECT_GT(CascadeTerrain.first, 0u);
	std::cout << "[ TERRAIN SHADOW ] single_median_ns="
		<< SingleLogical.first << ",single_p95_ns=" << SingleLogical.second
		<< ",cascade_median_ns=" << CascadeLogical.first
		<< ",cascade_p95_ns=" << CascadeLogical.second
		<< ",terrain_median_ns=" << CascadeTerrain.first
		<< ",terrain_p95_ns=" << CascadeTerrain.second
		<< ",membership=" << ThreeCascades.Telemetry.DirectionalShadow.ShadowMembershipPopcount
		<< ",patch_classifications="
		<< ThreeCascades.Telemetry.DirectionalShadow.ShadowTerrainPatchClassificationTests << "\n";

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(91));
	Durin::FlushRenderingCommands();
	TimingQueries.clear();
	RendererLifecycle.Shutdown();
	Durin::SetViewRenderTelemetrySink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
