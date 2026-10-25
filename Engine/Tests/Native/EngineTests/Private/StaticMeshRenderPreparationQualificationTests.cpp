#include "StaticMeshRenderPreparationTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "SceneTestAccess.h"
#include "SceneInfo.h"
#include "SceneView.h"

using namespace Durin::Tests::Preparation;

TEST(FStaticMeshRenderPreparationQualificationTests,
	RecordsMixedGeometryPreparationBaseline)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	Durin::Testing::InitializeDObjectSystemForTests();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	auto RenderData = MakeRenderData();
	const std::vector<Durin::FMaterialRenderProxyRef> Materials{
		MakeMaterial(Durin::EMaterialBlendMode::Opaque),
		MakeMaterial(Durin::EMaterialBlendMode::Masked),
		MakeMaterial(Durin::EMaterialBlendMode::Translucent),
		MakeMaterial(Durin::EMaterialBlendMode::Opaque, true)};
	Durin::FSceneTestOwner SceneOwner;
	auto& Scene = *SceneOwner;
	Durin::EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(RenderData->InitResources(CommandList));
		});
	Durin::FlushRenderingCommands();

	constexpr size_t PrimitiveCount = 64;
	for (size_t Index = 0; Index < PrimitiveCount; ++Index)
	{
		std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy;
		if (Index % 2 == 0)
		{
			Proxy = std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData.get(), Materials);
		}
		else
		{
			Durin::FSplineMeshRenderDynamicData Dynamic{
				.Params = {},
				.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0})};
			Dynamic.Params.EndPosition = {100.0, 30.0, 10.0};
			Dynamic.Params.EndTangent = {80.0, 0.0, 10.0};
			Dynamic.Params.SourceForwardMin = -1.0;
			Dynamic.Params.SourceForwardMax = 1.0;
			Proxy = std::make_unique<Durin::FSplineMeshSceneProxy>(
				RenderData.get(), Materials, Dynamic);
		}
		Durin::FSceneInterfaceTestAccess::AddPrimitiveProxy(
			Scene, Durin::FPrimitiveComponentId(1000 + Index), std::move(Proxy),
			Durin::FMatrix(1.0));
	}
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			constexpr size_t WarmupFrames = 30;
			constexpr size_t MeasuredFrames = 120;
			// This is owned top-level container capacity, not allocator or
			// transitive material/RHI memory accounting.
			auto ContainerBytes = [](const Durin::FPreparedStaticMeshView& Prepared) {
				return Prepared.Primitives.capacity() * sizeof(Durin::FPreparedStaticMeshPrimitive)
					+ (Prepared.Opaque.capacity() + Prepared.Masked.capacity()
						+ Prepared.Translucent.capacity()) * sizeof(Durin::FPreparedStaticMeshDraw)
					+ (Prepared.RequestedLODHistogram.capacity()
						+ Prepared.SelectedLODHistogram.capacity()) * sizeof(size_t);
			};
			RecordProperty("baseline_primitives", std::to_string(PrimitiveCount));
			RecordProperty("baseline_draws", "256");
			RecordProperty("baseline_warmup_frames", std::to_string(WarmupFrames));
			RecordProperty("baseline_measured_frames", std::to_string(MeasuredFrames));
			RecordProperty("baseline_timing_authority", "diagnostic");
			for (size_t Run = 0; Run < 3; ++Run)
			{
				std::array<double, MeasuredFrames> Nanoseconds{};
				size_t PeakContainerBytes = 0;
				for (size_t Frame = 0; Frame < WarmupFrames + MeasuredFrames; ++Frame)
				{
					const auto Start = std::chrono::steady_clock::now();
					const auto Prepared = Durin::PrepareStaticMeshView_RenderThread(
						CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
						Durin::ERasterMode::Solid);
					const auto End = std::chrono::steady_clock::now();
					ASSERT_EQ(Prepared.Primitives.size(), PrimitiveCount);
					ASSERT_EQ(Prepared.Opaque.size(), 128u);
					ASSERT_EQ(Prepared.Masked.size(), 64u);
					ASSERT_EQ(Prepared.Translucent.size(), 64u);
					ASSERT_EQ(Prepared.RejectedPrimitives, 0u);
					ASSERT_EQ(Prepared.PreparedSplinePrimitives, 32u);
					if (Frame >= WarmupFrames)
					{
						Nanoseconds[Frame - WarmupFrames] =
							std::chrono::duration<double, std::nano>(End - Start).count();
						PeakContainerBytes = std::max(PeakContainerBytes, ContainerBytes(Prepared));
					}
				}
				std::ranges::sort(Nanoseconds);
				const double Median = (Nanoseconds[59] + Nanoseconds[60]) / 2.0;
				const double P95 = Nanoseconds[113]; // Nearest rank: ceil(0.95 * 120).
				std::cout << std::format("[GeometryPreparationBaseline] run={} median_ns={} p95_ns={} peak_container_bytes={} authority=diagnostic\n",
					Run + 1, Median, P95, PeakContainerBytes);
				RecordProperty(std::format("baseline_run_{}_median_ns", Run + 1), std::to_string(Median));
				RecordProperty(std::format("baseline_run_{}_p95_ns", Run + 1), std::to_string(P95));
				RecordProperty(std::format("baseline_run_{}_peak_container_bytes", Run + 1), std::to_string(PeakContainerBytes));
			}
		});
	Durin::FlushRenderingCommands();
	SceneOwner.Reset();
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](Durin::FRHICommandListImmediate&) { RenderData->ReleaseResources(); });
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
