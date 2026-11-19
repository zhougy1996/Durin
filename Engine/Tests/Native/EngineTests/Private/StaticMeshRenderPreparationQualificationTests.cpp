#include "NativeAssetRuntimeTestSupport.h"
#include "StaticMeshRenderPreparationTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "QualificationViewPreparation.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/StaticMeshBatchPublication.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/ViewRenderTelemetry.h"
#include "SceneTestAccess.h"
#include "SceneInfo.h"
#include "SceneView.h"
#include "Threading/TaskRuntime.h"

using namespace Durin::Tests::Preparation;

namespace
{
	using FSampleClock = std::chrono::steady_clock;

	auto SampleNanoseconds(FSampleClock::duration Duration) -> uint64
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count();
	}
}

TEST(FStaticMeshRenderPreparationQualificationTests,
	RecordsMixedGeometryPreparationBaseline)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	Durin::Testing::InitializeAssetSystemForTests();
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

	constexpr size_t WarmupFrames = 30;
	constexpr size_t MeasuredFrames = 120;
	constexpr size_t Runs = 3;
	struct FPreparationSample
	{
		double Nanoseconds = 0;
		size_t ContainerBytes = 0;
	};
	std::array<std::array<FPreparationSample, MeasuredFrames>, Runs> Samples{};
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
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
			for (size_t Run = 0; Run < Samples.size(); ++Run)
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
						const size_t SampleIndex = Frame - WarmupFrames;
						auto& Sample = Samples[Run][SampleIndex];
						Sample.Nanoseconds =
							std::chrono::duration<double, std::nano>(End - Start).count();
						Sample.ContainerBytes = ContainerBytes(Prepared);
						Nanoseconds[SampleIndex] = Sample.Nanoseconds;
						PeakContainerBytes = std::max(PeakContainerBytes, Sample.ContainerBytes);
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
	if (HasFatalFailure()) return;

	const auto Path = Durin::Testing::CreateTestFixtureDirectory("GeometryPreparationBaseline") / "samples.csv";
	std::string Csv = "run,sample,nanoseconds,container_bytes\n";
	for (size_t Run = 0; Run < Samples.size(); ++Run)
	{
		for (size_t Index = 0; Index < Samples[Run].size(); ++Index)
		{
			const auto& Sample = Samples[Run][Index];
			Csv += std::format("{},{},{},{}\n", Run + 1, Index, Sample.Nanoseconds, Sample.ContainerBytes);
		}
	}
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Csv)), Path));
	std::cout << "[GeometryPreparationBaseline] csv=" << Path.generic_string() << '\n';
}

TEST(FStaticMeshRenderPreparationQualificationTests, RecordsDisjointViewCacheChurnBaseline)
{
	using namespace Durin;
	constexpr size_t GridWidth = 32;
	constexpr size_t KeysPerView = GridWidth * GridWidth;
	constexpr uint32 Extent = 192;
	constexpr int WarmupFrames = 30;
	constexpr int MeasuredFrames = 120;
	constexpr int Runs = 3;
	constexpr int RevisitGap = 8;
	const std::array ViewCenters{-128.0, 128.0};
	ASSERT_TRUE(IsTaskSchedulerRunning() || InitializeTaskScheduler(2));
	Testing::InitializeAssetSystemForTests();
	ASSERT_EQ(GDynamicRHI, nullptr);
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("DisjointViewCacheChurnQualification");
	RendererLifecycle.Start(Renderer);

	std::vector<std::unique_ptr<FStaticMeshRenderData>> Meshes;
	Meshes.reserve(ViewCenters.size() * KeysPerView);
	for (size_t Index = 0; Index < ViewCenters.size() * KeysPerView; ++Index)
	{
		auto Mesh = MakeRenderData();
		Mesh->MaterialSlots.resize(1);
		Mesh->LODResources[0].Sections.resize(1);
		Mesh->LODResources[0].IndexBuffer.Init({0, 1, 2});
		Mesh->RecalculateBounds();
		Meshes.push_back(std::move(Mesh));
	}
	FSceneTestOwner Scene;
	const std::vector Materials{MakeMaterial(EMaterialBlendMode::Opaque, true)};
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](FRHICommandListImmediate& Commands) {
			for (auto& Mesh : Meshes) ASSERT_TRUE(Mesh->InitResources(Commands));
		});
	FlushRenderingCommands();
	if (!HasFatalFailure())
	{
		for (size_t ViewIndex = 0; ViewIndex < ViewCenters.size(); ++ViewIndex)
		{
			for (size_t Index = 0; Index < KeysPerView; ++Index)
			{
				const size_t MeshIndex = ViewIndex * KeysPerView + Index;
				const FVector3 Offset(ViewCenters[ViewIndex] + 3.0 * (Index % GridWidth) - 46.5,
					3.0 * (Index / GridWidth) - 46.5, 0.5);
				FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(MeshIndex + 1),
					std::make_unique<FStaticMeshSceneProxy>(Meshes[MeshIndex].get(), Materials), Math::TranslationMatrix(Offset));
			}
		}
		FlushRenderingCommands();
	}

	std::string Csv = "scenario,run,sample,view,render_view_ns,visited_primitives,visible_primitives,prepared_sections,native_draws,"
		"description_reuses,description_builds,descriptions_at_start,descriptions_before_trim,descriptions_after_trim,"
		"facts_at_start,facts_before_trim,facts_after_trim,sections_at_start,sections_before_trim,sections_after_trim,"
		"gt_frame_ns,rt_frame_ns,cache_bytes_before_trim,cache_bytes_after_trim,cache_evictions\n";
	const char* MeasureMemory = std::getenv("DURIN_TEST_DISJOINT_VIEW_MEMORY");
	const bool bMeasureMemory = MeasureMemory && std::string_view(MeasureMemory) == "1";
	auto RunFixture = [&] {
		std::array<FSceneView, 2> Views;
		FTextureRHIRef Target;
		FSceneViewRenderOptions Options;
		Options.bEnableDynamicInstancing = false;
		Options.bEnableGPUCulling = false;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			std::unordered_set<uint64> GeometryIds;
			std::unordered_set<const FStaticMeshBatchInputs*> StaticInputs;
			FStaticMeshBatchCaptureContext Capture;
			for (size_t Index = 0; Index < Views.size(); ++Index)
			{
				auto& View = Views[Index];
				View.ViewportWidth = View.ViewportHeight = Extent;
				View.Settings.Mode.RenderMode = ERenderMode::Unlit;
				View.ViewLocation = FVector3(ViewCenters[Index], 0.0, 0.0);
				View.ViewMatrix = Math::TranslationMatrix(-View.ViewLocation);
				View.ProjectionMatrix[0][0] = View.ProjectionMatrix[1][1] = 1.0 / 50.0;
				View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;

				FViewRenderTelemetry VisibilityTelemetry;
				const auto Visible = PrepareSceneVisibility(*Scene, View, VisibilityTelemetry);
				ASSERT_EQ(Visible.SceneInfos.size(), KeysPerView);
				ASSERT_EQ(VisibilityTelemetry.Visibility.FrustumCulledPrimitives, KeysPerView);
				for (const auto* Info : Visible.SceneInfos)
				{
					ASSERT_GT(Info->GetId().Value, Index * KeysPerView);
					ASSERT_LE(Info->GetId().Value, (Index + 1) * KeysPerView);
					const auto Batch = static_cast<const FStaticMeshSceneProxy&>(Info->GetProxy())
						.CaptureStaticMeshBatches_RenderThread(Capture);
					ASSERT_TRUE(Batch && Batch->LODs.size() == 1 && Batch->LODs.front());
					const auto& Inputs = Batch->LODs.front();
					ASSERT_EQ(Inputs->Geometry->GetElements().size(), 1u);
					ASSERT_TRUE(GeometryIds.insert(Inputs->Geometry->GetRecordId()).second);
					ASSERT_TRUE(StaticInputs.insert(Inputs.get()).second);
				}
			}

			Target = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("DisjointViewCacheChurn", Extent, Extent, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
			for (const auto& View : Views)
				ASSERT_TRUE(Tests::PrepareQualificationView(Renderer, Commands, {Scene.Get(), View, Target, false, Options}));
		});
		FlushRenderingCommands();
		if (HasFatalFailure()) return;

		struct FFrameSample
		{
			FViewRenderTelemetry Telemetry;
			uint64 GTFrame = 0, RTFrame = 0, RenderView = 0;
		};
		FRenderCommandFence Fence;
		auto Render = [&](size_t View, FFrameSample& Frame, bool bMeasured) {
			const auto GTStart = FSampleClock::now();
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&, View, bMeasured](FRHICommandListImmediate& Commands) {
				static thread_local std::optional<FViewRenderTelemetry> Observed;
				Observed.reset();
				SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Observed = Value; });
				struct FResetTelemetrySink
				{
					~FResetTelemetrySink() { SetViewRenderTelemetrySink(nullptr); Observed.reset(); }
				} ResetSink;
				FScopedRendererQualificationPolicy Qualification({.bMeasurePreparationCacheMemory = bMeasureMemory && bMeasured});
				const auto RTStart = FSampleClock::now();
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
				Renderer.BeginFrame_RenderThread(Commands);
				const auto Start = FSampleClock::now();
				const auto Result = Renderer.RenderView(Commands, {Scene.Get(), Views[View], Target, false, Options});
				Frame.RenderView = SampleNanoseconds(FSampleClock::now() - Start);
				GDynamicRHI->RHIEndFrame_RenderThread(Commands);
				Frame.RTFrame = SampleNanoseconds(FSampleClock::now() - RTStart);
				ASSERT_EQ(Result, ERenderViewResult::Success);
				ASSERT_TRUE(Observed.has_value());
				Frame.Telemetry = std::move(*Observed);
				const auto& Visibility = Frame.Telemetry.Visibility;
				const auto& Mesh = Frame.Telemetry.StaticMesh;
				ASSERT_EQ(Visibility.SubmittedPrimitives, Views.size() * KeysPerView);
				ASSERT_EQ(Visibility.VisiblePrimitives, KeysPerView);
				ASSERT_EQ(Visibility.FrustumCulledPrimitives, KeysPerView);
				ASSERT_EQ(Mesh.PreparedStaticMeshSections, KeysPerView);
				ASSERT_EQ(Mesh.NativeDrawSubmissions, KeysPerView);
				ASSERT_EQ(Mesh.DrawDescriptionReuses + Mesh.DrawDescriptionBuilds, KeysPerView);
			});
			Fence.BeginFence();
			Fence.Wait();
			Frame.GTFrame = SampleNanoseconds(FSampleClock::now() - GTStart);
		};

		auto RunPhase = [&](const char* Scenario, int Run, int Warmup, int Count, auto SelectView) {
			std::vector<FFrameSample> Frames(Count);
			FFrameSample WarmupFrame;
			for (int Sample = -Warmup; Sample < 0 && !HasFatalFailure(); ++Sample)
				Render(SelectView(Sample), WarmupFrame, false);
			if (HasFatalFailure()) return;
			FlushRenderingCommands();
			for (int Sample = 0; Sample < Count && !HasFatalFailure(); ++Sample)
				Render(SelectView(Sample), Frames[Sample], true);
			if (HasFatalFailure()) return;
			for (int Sample = 0; Sample < Count; ++Sample)
			{
				const auto& Frame = Frames[Sample];
				const auto& Visibility = Frame.Telemetry.Visibility;
				const auto& Mesh = Frame.Telemetry.StaticMesh;
				EXPECT_EQ(Mesh.DrawDescriptionBuilds, 0u) << Scenario << " frame " << Sample;
				EXPECT_EQ(Mesh.DrawDescriptionReuses, KeysPerView);
				if (bMeasureMemory)
				{
					ASSERT_GT(Mesh.PreparationCache.AccountedBytesAfterTrim, 0u);
					ASSERT_GE(Mesh.PreparationCache.AccountedBytesBeforeTrim, Mesh.PreparationCache.AccountedBytesAfterTrim);
					ASSERT_LE(Mesh.PreparationCache.AccountedBytesBeforeTrim, FMeshDrawDescriptionCache::MaxBytes);
				}
				const auto& [StartEntries, BeforeTrim, AfterTrim, BytesBefore, BytesAfter, Evictions] = Mesh.PreparationCache;
				Csv += std::format("{},{},{},{},{},{},{},{},{},{},{}" ",{},{},{}" ",{},{},{}" ",{},{},{}" ",{},{},{},{},{}\n",
					Scenario, Run, Sample, SelectView(Sample) == 0 ? 'A' : 'B', Frame.RenderView,
					Visibility.SubmittedPrimitives, Visibility.VisiblePrimitives, Mesh.PreparedStaticMeshSections,
					Mesh.NativeDrawSubmissions, Mesh.DrawDescriptionReuses, Mesh.DrawDescriptionBuilds,
					StartEntries.DrawDescriptions, BeforeTrim.DrawDescriptions, AfterTrim.DrawDescriptions,
					StartEntries.StaticFacts, BeforeTrim.StaticFacts, AfterTrim.StaticFacts,
					StartEntries.StaticSections, BeforeTrim.StaticSections, AfterTrim.StaticSections,
					Frame.GTFrame, Frame.RTFrame, BytesBefore, BytesAfter, Evictions);
			}
		};
		RecordProperty("keys_per_view", std::to_string(KeysPerView));
		RecordProperty("authority", "diagnostic");
		RecordProperty("cache_memory", bMeasureMemory ? "enabled" : "disabled");
		for (int Run = 1; Run <= Runs && !HasFatalFailure(); ++Run)
		{
			RunPhase("same_a", Run, WarmupFrames, MeasuredFrames, [](int) { return 0u; });
			RunPhase("same_b", Run, WarmupFrames, MeasuredFrames, [](int) { return 1u; });
			RunPhase("alternate", Run, WarmupFrames, MeasuredFrames, [](int Sample) { return (Sample + WarmupFrames) % 2; });
			RunPhase("revisit", Run, RevisitGap, 2, [](int Sample) { return Sample < 0 ? 1u : 0u; });
		}
	};
	if (!HasFatalFailure()) RunFixture();
	Scene.Reset();
	FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate&) {
		for (auto& Mesh : Meshes) Mesh->ReleaseResources();
	});
	FlushRenderingCommands();
	ShutdownRenderingThread();
	RHIExit();
	if (HasFatalFailure()) return;

	const auto Path = Testing::CreateTestFixtureDirectory("DisjointViewCacheChurnBaseline") / "samples.csv";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Csv)), Path));
	std::cout << "[DisjointViewCacheChurnBaseline] keys_per_view=" << KeysPerView
		<< " warmup=" << WarmupFrames << " samples=" << MeasuredFrames << " runs=" << Runs
		<< " revisit_gap=" << RevisitGap << " cache_memory=" << bMeasureMemory
		<< " authority=diagnostic csv=" << Path.generic_string() << '\n';
}
