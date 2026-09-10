#include "EngineTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "StaticMesh/StaticMeshResources.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "SceneTestAccess.h"
#include "SceneView.h"
#include "VulkanCreationQualificationSupport.h"
#include "PCH.VulkanRHI.h"
#include "VulkanDynamicRHI.h"
#include "Misc/FileHelper.h"
#include "DObject/ObjectLifecycle.h"
#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		constexpr int32 CaptureSize = 256;
		const FVector3 SurfaceColor{0.25, 0.5, 0.75};
		struct FMaterialMeasurementCommand
		{
			static constexpr auto GetName() -> const char* { return "MaterialCreationMeasurement"; }
		};

		auto MakeMeasurementMesh() -> std::unique_ptr<FStaticMeshRenderData>
		{
			auto Data = std::make_unique<FStaticMeshRenderData>();
			Data->MaterialSlots = {{"Surface", 0}};
			auto& LOD = Data->LODResources.emplace_back();
			const std::vector<FVector3f> Positions{{-1,-1,0}, {1,-1,0}, {1,1,0}, {-1,1,0}};
			LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
			LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
				std::vector<FVector3f>(4, {0,0,1}), std::vector<FVector4f>(4, {1,0,0,1}));
			std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
			UVs[0] = {{0,0}, {1,0}, {1,1}, {0,1}};
			LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), 4, 1);
			LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(4, FVector4f(1)), 4);
			LOD.IndexBuffer.Init({0,1,2,0,2,3});
			LOD.Sections.push_back({.Name="Surface", .FirstIndex=0, .IndexCount=6,
				.MinVertexIndex=0, .MaxVertexIndex=3, .MaterialSlotIndex=0,
				.LocalBounds=FBox({-1,-1,0},{1,1,0})});
			LOD.LocalBounds = LOD.Sections[0].LocalBounds;
			LOD.NumTexCoords = 1;
			LOD.bHasColorVertexData = true;
			Data->LODVertexFactories.resize(1);
			Data->RecalculateBounds();
			return Data;
		}

		// Always release scene/material and queued work before renderer and device teardown.
		class FMaterialMeasurementLifetime
		{
		public:
			FMaterialMeasurementLifetime() : RendererLifecycle("MaterialCreationMeasurement") {}
			~FMaterialMeasurementLifetime()
			{
				Scene.reset();
				Proxy = {};
				if (Material) MarkAsGarbage(Material);
				CollectGarbage();
				if (bRendererStarted) RendererLifecycle.Shutdown();
				if (bRenderingThreadStarted)
				{
					EnqueueRenderCommand<FMaterialMeasurementCommand>([this](FRHICommandListImmediate&) {
						Target = nullptr;
						if (Mesh) Mesh->ReleaseResources();
					});
					FlushRenderingCommands();
					ShutdownRenderingThread();
				}
				if (GDynamicRHI) RHIExit();
			}
			FRendererModule Renderer;
			FModuleTestHarness RendererLifecycle;
			std::unique_ptr<FSceneTestOwner> Scene;
			std::unique_ptr<FStaticMeshRenderData> Mesh;
			DMaterial* Material = nullptr;
			FMaterialRenderProxyRef Proxy;
			FTextureRHIRef Target;
			bool bRenderingThreadStarted = false;
			bool bRendererStarted = false;
		};

		class FMaterialCreationQualificationTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				ASSERT_TRUE(VulkanRHI::PrepareCreationQualificationValidationLayer());
				ASSERT_TRUE(VulkanRHI::ConfigureCreationQualificationAffinity());
				ASSERT_TRUE(VulkanRHI::ConfigureCreationQualificationHighQos());
				InitializeDObjectSystem();
				ASSERT_TRUE(InitializeAssetCompilingManager());
				if (const char* Mode = std::getenv("DURIN_RHI_EXECUTION")) PreviousMode = Mode;
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
				FModuleManager::Get().LoadModule("RenderCore");
			}
			auto TearDown() -> void override
			{
				VulkanRHI::SetVulkanPipelineCachePathForTest({});
				ShutdownAssetCompilingManager();
				_putenv_s("DURIN_RHI_EXECUTION", PreviousMode ? PreviousMode->c_str() : "");
			}
			std::optional<std::string> PreviousMode;
		};
	}

	TEST_F(FMaterialCreationQualificationTests, FirstVisibleFrameAndFollowing120Frames)
	{
		using namespace VulkanRHI;
		const auto Output = Testing::CreateTestFixtureDirectory("MaterialCreationTiming");
		const auto CachePath = Output / "PipelineCache-v1.bin";
		SetVulkanPipelineCachePathForTest(CachePath);
		std::ofstream Manifest(Output / "scene.txt");
		Manifest << "schema=1\nscene=one canonical unlit opaque two-sided quad\n"
			<< "positions=(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)\nindices=0,1,2,0,2,3\n"
			<< "transform=translate(0,0,-0.5)*scale(0.75,0.75,1)\n"
			<< "view=identity; projection=identity with Z=-Z; culling=disabled\n"
			<< "color=" << SurfaceColor.x << ',' << SurfaceColor.y << ',' << SurfaceColor.z << '\n'
			<< "output=" << CaptureSize << 'x' << CaptureSize << " SRGBA8_UNORM\n"
			<< "frames=first+120; frame includes producer enqueue, render/replay, submit and GPU completion\n"
			<< "prewarm_trigger=accepted material and mesh resources known; baseline has no PSO prewarm\n"
			<< "application_pipeline_cache=cold per lifetime; driver_private_cache=uncontrolled\n";
		WriteCreationHost(Manifest);
		std::ofstream Frames(Output / "frames.csv");
		Frames << "round,frame,producer_begin_ns,render_begin_ns,render_end_ns,gpu_complete_ns,producer_complete_ns,prewarm_trigger_ns,synchronous_operations,submission_serials,graphics_native,compute_native,queue_entries_peak,queue_payload_peak"
			<< CreationMemoryColumns << ",replay_complete_ns\n";
		std::ofstream Requests(Output / "requests.csv");
		Requests << "round,frame," << CreationRequestColumns << '\n';
		std::vector<FByteBuffer> FrozenShaders;
		for (uint32 Round = 0; Round < 30; ++Round)
		{
			std::filesystem::remove(CachePath);
			FMaterialMeasurementLifetime Life;
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			if (Round == 0) Manifest << GetVulkanDeviceDescriptionForTiming();
			InitRenderingThread(); Life.bRenderingThreadStarted = true;
			Life.RendererLifecycle.Start(Life.Renderer); Life.bRendererStarted = true;
			Life.Mesh = MakeMeasurementMesh();
			EnqueueRenderCommand<FMaterialMeasurementCommand>([&](FRHICommandListImmediate& Cmd) {
				ASSERT_TRUE(Life.Mesh->InitResources(Cmd));
				auto Desc = FRHITextureCreateDesc::Create2D("MaterialFirstUse", CaptureSize, CaptureSize,
					EPixelFormat::SRGBA8_UNORM).SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
				Life.Target = GDynamicRHI->RHICreateTexture(Cmd, Desc);
			});
			FlushRenderingCommands();
			ASSERT_TRUE(Life.Target);
			Life.Material = NewObject<DMaterial>(nullptr, std::format("MeasuredMaterial{}", Round));
			ASSERT_NE(Life.Material, nullptr);
			FMaterialProgramValidationResult Validation;
			ASSERT_TRUE(Life.Material->SetMaterialProgram(MakePBRMaterialProgram(), Validation));
			ASSERT_TRUE(Life.Material->SetStaticProperties({.BlendMode=EMaterialBlendMode::Opaque,
				.ShadingModel=EMaterialShadingModel::Unlit, .bTwoSided=true}));
			ASSERT_TRUE(Life.Material->SetVectorParameterValue(MaterialParameters::BaseColorName(), SurfaceColor));
			ASSERT_TRUE(Life.Material->SetVectorParameterValue(MaterialParameters::EmissiveName(), SurfaceColor));
			if (Life.Material->GetMaterialCompileStatus().State == EMaterialCompileState::NeverRequested)
				ASSERT_TRUE(RequestMaterialRecompile(*Life.Material));
			DObject* Object = Life.Material;
			FAssetCompilingManager::Get().FinishCompilationForObjects(std::span<DObject* const>(&Object, 1));
			ASSERT_TRUE(Life.Material->GetMaterialCompileStatus().IsCurrent());
			const auto Program = Life.Material->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program);
			if (Round == 0)
			{
				for (size_t Index = 0; Index < Program->CompiledShaders.size(); ++Index)
				{
					FrozenShaders.push_back(*Program->CompiledShaders[Index].Code);
					ASSERT_TRUE(FFileHelper::SaveArrayToFile(FrozenShaders.back(),
						Output / std::format("material-shader{}.spv", Index)));
				}
			}
			ASSERT_EQ(FrozenShaders.size(), Program->CompiledShaders.size());
			for (size_t Index = 0; Index < FrozenShaders.size(); ++Index)
				ASSERT_EQ(FrozenShaders[Index], *Program->CompiledShaders[Index].Code);
			Life.Proxy = Life.Material->GetMaterialRenderProxy();
			Life.Scene = std::make_unique<FSceneTestOwner>();
			ASSERT_TRUE(FSceneInterfaceTestAccess::TryAddPrimitiveProxy(**Life.Scene, FPrimitiveSceneId(1),
				std::make_unique<FStaticMeshSceneProxy>(Life.Mesh.get(), std::vector<FMaterialRenderProxyRef>{Life.Proxy}, 1),
				Math::TranslationMatrix(FVector3(0,0,-0.5)) * Math::ScaleMatrix(FVector3(0.75,0.75,1))));
			FlushRenderingCommands();
			const auto PrewarmTrigger = VulkanCreationTimestamp();
			FByteBuffer FirstPixels;
			for (uint32 Frame = 0; Frame <= 120; ++Frame)
			{
				std::array<uint64, 4> RenderTimes{};
				const auto Before = GCommandListExecutor.GetStats();
				const auto CacheBefore = GDynamicRHI->RHIGetPipelineCacheStatistics();
				FCreationMemorySampler MemorySampler;
				BeginVulkanCreationTimingCapture(8192);
				const auto ProducerStart = VulkanCreationTimestamp();
				EnqueueRenderCommand<FMaterialMeasurementCommand>([&](FRHICommandListImmediate& Cmd) {
					RenderTimes[0] = VulkanCreationTimestamp();
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
					FSceneView View;
					View.ViewMatrix = FMatrix(1);
					View.ProjectionMatrix = FMatrix(1);
					View.ProjectionMatrix[2][2] = -1;
					View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
					View.ViewportWidth = CaptureSize; View.ViewportHeight = CaptureSize;
					View.Settings.Mode.RenderMode = ERenderMode::Unlit;
					View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
					EXPECT_EQ(Life.Renderer.RenderView(Cmd, Life.Scene->Get(), View, Life.Target, false, {}), ERenderViewResult::Success);
					GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
					RenderTimes[1] = VulkanCreationTimestamp();
					Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread, ERHISubmitFlags::SubmitToGPU);
					RenderTimes[3] = VulkanCreationTimestamp();
					GDynamicRHI->RHIBlockUntilGPUIdle();
					RenderTimes[2] = VulkanCreationTimestamp();
				});
				FlushRenderingCommands();
				const auto ProducerEnd = VulkanCreationTimestamp();
				uint64 Dropped = 0;
				const auto Samples = EndVulkanCreationTimingCapture(Dropped);
				const auto Memory = MemorySampler.Stop();
				ASSERT_EQ(Dropped, 0u);
#ifdef _WIN32
				ASSERT_TRUE(Memory.bAvailable);
#endif
				Frames << Round << ',' << Frame << ',' << ProducerStart << ',' << RenderTimes[0] << ','
					<< RenderTimes[1] << ',' << RenderTimes[2] << ',' << ProducerEnd << ',' << PrewarmTrigger;
				const auto After = GCommandListExecutor.GetStats();
				const auto CacheAfter = GDynamicRHI->RHIGetPipelineCacheStatistics();
				Frames << ',' << After.SynchronousOperationCount - Before.SynchronousOperationCount
					<< ',' << After.LastSubmittedSerial - Before.LastSubmittedSerial
					<< ',' << CacheAfter.GraphicsPipelines.NativeCreations - CacheBefore.GraphicsPipelines.NativeCreations
					<< ',' << CacheAfter.ComputePipelines.NativeCreations - CacheBefore.ComputePipelines.NativeCreations
					<< ',' << After.PeakQueueEntryCount << ',' << After.PeakQueuePayloadBytes;
				WriteCreationMemorySample(Frames, Memory); Frames << ',' << RenderTimes[3] << '\n';
				for (const auto& Sample : Samples)
				{
					ASSERT_TRUE(Sample.bSucceeded);
					Requests << Round << ',' << Frame << ',';
					WriteCreationRequest(Requests, Sample); Requests << '\n';
				}
				if (Frame == 0 || Frame == 120)
				{
					FByteBuffer Pixels;
					EnqueueRenderCommand<FMaterialMeasurementCommand>([&](FRHICommandListImmediate& Cmd) {
						ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd, Life.Target, 0, 0, Pixels));
					});
					FlushRenderingCommands();
					ASSERT_EQ(Pixels.size(), size_t(CaptureSize) * CaptureSize * 4);
					const size_t Center = (CaptureSize / 2 * CaptureSize + CaptureSize / 2) * 4;
					EXPECT_LT(std::to_integer<uint8>(Pixels[Center]), std::to_integer<uint8>(Pixels[Center+1]));
					EXPECT_LT(std::to_integer<uint8>(Pixels[Center+1]), std::to_integer<uint8>(Pixels[Center+2]));
					if (Frame == 0) FirstPixels = std::move(Pixels);
					else EXPECT_EQ(FirstPixels, Pixels);
				}
			}
		}
		ASSERT_TRUE(Frames.good()); ASSERT_TRUE(Requests.good()); ASSERT_TRUE(Manifest.good());
		std::cout << "Material creation timing output: " << Output << std::endl;
	}
}
