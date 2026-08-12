#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "RHICommandList.h"
#include "RHI.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRendererProfiling.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Terrain/TerrainHeightmap.h"

#include <gtest/gtest.h>

namespace
{
	Durin::FViewRenderCounters GCounters;
	std::vector<Durin::FGPUTimingQueryRHIRef>* GTimingQueries = nullptr;
	auto CaptureCounters(const Durin::FViewRenderCounters& Counters) -> void
	{
		GCounters = Counters;
	}
	auto CaptureTiming(const Durin::FGPUTimingQueryRHIRef& Query) -> void
	{
		if (GTimingQueries) GTimingQueries->push_back(Query);
	}

	struct FTerrainRenderCommand
	{
		static constexpr auto GetName() -> const char* { return "TerrainRenderValidation"; }
	};
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
	Renderer.StartupModule();
	Durin::SetViewRenderCounterSink(CaptureCounters);

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
		.ShadingModel = Durin::EMaterialShadingModel::Unlit,
		.bTwoSided = true};
	Publication.LocalLayer.Parameters.push_back({
		.Id = Durin::MaterialParameters::BaseColorId,
		.Type = Durin::EMaterialParameterType::Vector,
		.VectorValue = {0.8f, 0.2f, 0.1f}});
	ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
	Durin::FlushRenderingCommands();
	Durin::FTerrainPatchDescriptor Patch{
		.OriginX = 0, .OriginY = 0, .CellCountX = 2, .CellCountY = 2,
		.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 0.5})};
	Durin::FScene Scene;
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(Payload, 1, 0.5, 0.5,
			0.5, 0.0, std::vector<Durin::FTerrainPatchDescriptor>{Patch},
			Patch.LocalBounds, Material, 1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();

	auto Readback = std::make_shared<std::vector<Durin::uint8>>();
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene, Readback](Durin::FRHICommandListImmediate& CommandList) {
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
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
				Durin::ERenderViewResult::Success);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Target, 0, 0, *Readback));
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Readback->size(), 65u * 65u * 4u);
	EXPECT_TRUE(std::ranges::any_of(*Readback, [](Durin::uint8 Value) { return Value != 0; }));
	EXPECT_EQ(GCounters.VisibleTerrainCandidates, 1u);
	EXPECT_EQ(GCounters.TerrainPatchCandidates, 1u);
	EXPECT_EQ(GCounters.VisibleTerrainPatches, 1u);
	EXPECT_EQ(GCounters.PreparedTerrainTriangles, 8u);
	EXPECT_EQ(GCounters.TerrainHeightUploadBytes, 18u);
	EXPECT_EQ(GCounters.TerrainAttemptedDraws, 1u);
	EXPECT_EQ(GCounters.TerrainSuccessfulDraws, 1u);
	EXPECT_EQ(GCounters.TerrainRejectedDraws, 0u);

	constexpr Durin::uint32 MaximumSamples = 1025;
	std::vector<Durin::uint16> MaximumPlane(
		static_cast<size_t>(MaximumSamples) * MaximumSamples, 32768);
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> MaximumPayload;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(
		MaximumSamples, MaximumSamples, MaximumPlane, MaximumPayload, Error)) << Error;
	std::vector<Durin::FTerrainPatchDescriptor> MaximumPatches;
	MaximumPatches.reserve(256);
	for (Durin::uint32 Y = 0; Y < 1024; Y += 64)
		for (Durin::uint32 X = 0; X < 1024; X += 64)
			MaximumPatches.push_back({X, Y, 64, 64,
				Durin::FBox({X / 1024.0, Y / 1024.0, 0.25},
					{(X + 64) / 1024.0, (Y + 64) / 1024.0, 0.25})});
	Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(91),
		std::make_unique<Durin::FTerrainSceneProxy>(MaximumPayload, 2,
			1.0 / 1024.0, 1.0 / 1024.0, 0.5, 0.0,
			std::move(MaximumPatches), Durin::FBox({0.0, 0.0, 0.25}, {1.0, 1.0, 0.25}),
			Material, 1), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	constexpr Durin::uint32 WarmupFrames = 2;
	constexpr Durin::uint32 MeasuredFrames = 7;
	std::vector<Durin::FGPUTimingQueryRHIRef> TimingQueries;
	std::vector<double> CpuMilliseconds;
	GTimingQueries = &TimingQueries;
	Durin::SetSceneColorTimingQuerySink(CaptureTiming);
	Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
		[&Renderer, &Scene, &CpuMilliseconds](Durin::FRHICommandListImmediate& CommandList) {
			const auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"MaximumTerrainValidationColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource);
			Durin::FTextureRHIRef Target = Durin::GDynamicRHI->RHICreateTexture(CommandList, Desc);
			Durin::FSceneView View;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewportWidth = 17;
			View.ViewportHeight = 17;
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.Settings.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
			for (Durin::uint32 Frame = 0; Frame < WarmupFrames + MeasuredFrames; ++Frame)
			{
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Begin = std::chrono::steady_clock::now();
				EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}),
					Durin::ERenderViewResult::Success);
				const auto End = std::chrono::steady_clock::now();
				if (Frame >= WarmupFrames)
					CpuMilliseconds.push_back(std::chrono::duration<double, std::milli>(End - Begin).count());
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		});
	Durin::FlushRenderingCommands();
	Durin::SetSceneColorTimingQuerySink(nullptr);
	GTimingQueries = nullptr;
	for (Durin::uint32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		const bool Ready = TimingQueries.size() == WarmupFrames + MeasuredFrames
			&& std::ranges::all_of(TimingQueries, [](const auto& Query) {
				return Query->GetResult().State == Durin::ERHIGPUTimingResultState::Ready;
			});
		if (Ready) break;
		Durin::EnqueueRenderCommand<FTerrainRenderCommand>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GRenderFrameCounterRenderThread++;
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}
	EXPECT_EQ(GCounters.TerrainPatchCandidates, 256u);
	EXPECT_EQ(GCounters.VisibleTerrainPatches, 256u);
	EXPECT_EQ(GCounters.PreparedTerrainTriangles, 2'097'152u);
	EXPECT_EQ(GCounters.TerrainHeightUploadBytes, 0u);
	EXPECT_EQ(GCounters.TerrainHeightReuses, 256u);
	EXPECT_EQ(GCounters.TerrainAttemptedDraws, 256u);
	EXPECT_EQ(GCounters.TerrainSuccessfulDraws, 256u);
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
	if (!CpuMilliseconds.empty() && !GpuMilliseconds.empty())
		std::cout << "[ TERRAIN ] 1025x1025 Debug Vulkan GTX1060: cpu median="
			<< CpuMilliseconds[CpuMilliseconds.size() / 2] << "ms p95="
			<< CpuMilliseconds.back() << "ms; gpu median="
			<< GpuMilliseconds[GpuMilliseconds.size() / 2] << "ms p95="
			<< GpuMilliseconds.back() << "ms\n";

	Scene.RemovePrimitive(Durin::FPrimitiveSceneId(91));
	Durin::FlushRenderingCommands();
	TimingQueries.clear();
	Renderer.ShutdownModule();
	Durin::SetViewRenderCounterSink(nullptr);
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
