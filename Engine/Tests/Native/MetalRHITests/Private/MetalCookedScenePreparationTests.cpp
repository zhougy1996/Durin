#include "SceneRenderQualification.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialInstance.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "MetalShaderStatistics.h"
#include "Misc/FileHelper.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	TEST(FMetalCookedScenePreparationTests, CompilerFreePreparationSurvivesActivationAndDeviceRecreation)
	{
		setenv("DURIN_RHI_BACKEND", "metal", 1);
		if (!GIsGameThreadIdInitialized) { GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true; }
		InitializeDObjectSystem();
		FModuleManager::Get().LoadModule("RenderCore");
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		auto* Source = NewObject<DMaterial>(nullptr, "CookedSceneMaterial");
		AddToRoot(Source);
		auto CookProgram = [](DMaterialInterface& Material, FMaterialStaticProperties& Properties) -> std::shared_ptr<const FMaterialCompilerResult> {
			auto Cooked = Material.PrepareCookedProgram(ECookTargetPlatform::MacOS, {});
			EXPECT_TRUE(Cooked) << Cooked.Diagnostic;
			if (!Cooked) return nullptr;
			auto Read = Cooked->AcquireRead();
			EXPECT_TRUE(Read);
			if (!Read) return nullptr;
			std::shared_ptr<const FMaterialCompilerResult> Program;
			const auto Decoded = DecodeMaterialCookedProgramFamily(Read->GetBytes(), ECookTargetPlatform::MacOS,
				ECookTargetProfile::Game, GetMaterialQualityLevel(), ERHIFeatureLevel::SM5, {}, Properties, Program);
			EXPECT_TRUE(Decoded) << Decoded.error();
			return Program;
		};
		FMaterialStaticProperties Properties, MaskedProperties, InstanceProperties;
		auto Program = CookProgram(*Source, Properties);
		ASSERT_TRUE(Program);
		auto* InstanceSource = NewObject<DMaterialInstance>(nullptr, "CookedSceneInstance");
		AddToRoot(InstanceSource);
		ASSERT_TRUE(InstanceSource->SetParent(Source));
		auto InstanceProgram = CookProgram(*InstanceSource, InstanceProperties);
		ASSERT_TRUE(InstanceProgram);
		auto Mask = Properties; Mask.BlendMode = EMaterialBlendMode::Masked; Mask.bTwoSided = true;
		ASSERT_TRUE(Source->SetStaticProperties(Mask));
		auto MaskedProgram = CookProgram(*Source, MaskedProperties);
		ASSERT_TRUE(MaskedProgram);
		RemoveFromRoot(InstanceSource); MarkObjectHierarchyAsGarbage(InstanceSource);
		RemoveFromRoot(Source); MarkObjectHierarchyAsGarbage(Source); CollectGarbage();
		FByteBuffer Fixed;
		const auto Built = BuildCookedShaderLibrary(EShaderTargetPlatform::MacOS, EShaderTargetProfile::Game, Fixed);
		ASSERT_TRUE(Built) << Built.Diagnostic;
		const auto Root = std::filesystem::absolute(Testing::CreateTestFixtureDirectory("CompilerFreePreparedMetalScene"));
		std::filesystem::create_directories(Root / "Shaders");
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Fixed, Root / ShaderCookedLibraryRelativePath));
		FModuleManager::Get().ShutdownModule("ShaderBuild");
		ASSERT_EQ(IShaderBuildModule::Get(), nullptr);
		ShutdownShaderData();
		ASSERT_TRUE(InitializeShaderData(FShaderDataConfiguration::Cooked(Root, EShaderTargetPlatform::MacOS)));
		for (const char* Mode : {"inline", "threaded"})
		{
			setenv("DURIN_RHI_EXECUTION", Mode, 1);
			RHIInit(FRHIInitializationContext::Headless());
			ASSERT_NE(GDynamicRHI, nullptr);
			InitRenderingThread();
			FRendererModule Renderer;
			FModuleTestHarness Lifecycle("CookedScenePreparation");
			Lifecycle.Start(Renderer);
			auto Scene = Renderer.CreateScene();
			auto Geometry = std::make_unique<FStaticMeshRenderData>();
			Geometry->MaterialSlots = {{"Prepared", 0}};
			auto& LOD = Geometry->LODResources.emplace_back();
			LOD.VertexBuffers.PositionVertexBuffer.Init({{0, -40, -40}, {0, 40, -40}, {0, 40, 40}, {0, -40, 40}});
			LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(std::vector<FVector3f>(4, {-1, 0, 0}), std::vector<FVector4f>(4, {0, 1, 0, 1}));
			std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
			UVs[0] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), 4, 1);
			LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(4, FVector4f(1)), 4);
			LOD.IndexBuffer.Init(std::vector<uint32>{0, 1, 2, 0, 2, 3});
			LOD.Sections.push_back({.Name = "Prepared", .FirstIndex = 0, .IndexCount = 6, .MinVertexIndex = 0, .MaxVertexIndex = 3,
				.MaterialSlotIndex = 0, .LocalBounds = FBox({0, -40, -40}, {0, 40, 40})});
			LOD.LocalBounds = LOD.Sections[0].LocalBounds;
			Geometry->LODVertexFactories.resize(1); Geometry->RecalculateBounds();
			auto Material = MakeRefCount<FMaterialRenderProxy>();
			auto MaskedMaterial = MakeRefCount<FMaterialRenderProxy>();
			auto InstanceMaterial = MakeRefCount<FMaterialRenderProxy>();
			EnqueueRenderCommand("InstallCookedSceneResources", [&](FRHICommandListImmediate& Commands) {
				ASSERT_TRUE(Geometry->InitResources(Commands));
				Material->ApplyPublication_RenderThread({.StaticProperties = Properties, .CompiledProgram = Program});
				MaskedMaterial->ApplyPublication_RenderThread({.StaticProperties = MaskedProperties, .CompiledProgram = MaskedProgram});
				InstanceMaterial->ApplyPublication_RenderThread({.StaticProperties = InstanceProperties, .CompiledProgram = InstanceProgram});
			});
			FlushRenderingCommands();
			ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*static_cast<FScene*>(Scene.get()), FPrimitiveComponentId(40001),
				std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector<FMaterialRenderProxyRef>{Material}), Math::TranslationMatrix(FVector3{100, 0, 2510})));
			FlushRenderingCommands();
			FSplineMeshRenderDynamicData Spline;
			Spline.LocalBounds = LOD.LocalBounds;
			Spline.Params.ForwardAxis = ESplineMeshAxis::Y;
			Spline.Params.SourceForwardMin = -40; Spline.Params.SourceForwardMax = 40;
			Spline.Params.StartPosition = {0, -40, 0}; Spline.Params.EndPosition = {0, 40, 0};
			Spline.Params.StartTangent = Spline.Params.EndTangent = {0, 80, 0};
			ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*static_cast<FScene*>(Scene.get()), FPrimitiveComponentId(40002),
				std::make_unique<FSplineMeshSceneProxy>(Geometry.get(), std::vector<FMaterialRenderProxyRef>{MaskedMaterial}, Spline),
				Math::TranslationMatrix(FVector3{100, 45, 2510})));
			auto Instances = std::make_shared<std::vector<FStaticMeshRenderInstance>>();
			Instances->push_back({.Id = 1});
			Instances->push_back({.Id = 2, .LocalToPrimitive = Math::TranslationMatrix(FVector3{0, 20, 0})});
			ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*static_cast<FScene*>(Scene.get()), FPrimitiveComponentId(40003),
				std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector<FMaterialRenderProxyRef>{InstanceMaterial}, Instances),
				Math::TranslationMatrix(FVector3{100, -45, 2510})));
			ASSERT_TRUE(FSceneInterfaceTestAccess::TryAddLightProxy(*static_cast<FScene*>(Scene.get()),
				std::make_unique<FDirectionalLightSceneProxy>(FLightComponentId(40004), FDirectionalLightSceneData{.Intensity = 1, .AmbientIntensity = 0.2f})));
			FlushRenderingCommands();
			auto Preparation = FRenderResourcePreparation::Create();
			auto Peer = FRenderResourcePreparation::Create();
			EnqueueRenderCommand("PrepareCookedScene", [&](FRHICommandListImmediate& Commands) {
				auto Output = GDynamicRHI->RHICreateTexture(Commands, FRHITextureCreateDesc::Create2D("PreparedScene", 96, 64, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Output);
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
				auto View = MakeSceneCloudView(96, 64);
				View.Settings.Mode.RenderMode = ERenderMode::Lit;
				View.Settings.AmbientOcclusion.bEnabled = true;
				View.Settings.DirectionalShadow.bEnableContactShadows = true;
				const auto Initial = GetMetalShaderCreationStatistics();
				const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
				const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				do {
					Renderer.PrepareViewResources(Commands, {Scene.get(), View, Output, false, {}}, Preparation);
					Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					std::this_thread::yield();
				} while (Preparation.GetProgress().State == ERenderResourcePreparationState::Pending && std::chrono::steady_clock::now() < Deadline);
				ASSERT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
				EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
				const auto Prepared = GetMetalShaderCreationStatistics();
				EXPECT_GT(Prepared.BinaryLoadAttempts, Initial.BinaryLoadAttempts);
				EXPECT_GT(Prepared.ComputePipelinesCreated, Initial.ComputePipelinesCreated);
				EXPECT_EQ(Prepared.SourceCompileAttempts, Initial.SourceCompileAttempts);
				FRenderResourcePreparationUseScope Use(Preparation);
				FSceneViewStatistics Statistics;
				EXPECT_EQ(Renderer.RenderView(Commands, {Scene.get(), View, Output, false, {}, &Statistics}), ERenderViewResult::Success);
				EXPECT_GT(Statistics.Summary.DrawCalls, 0u);
				EXPECT_GT(Statistics.StaticMesh.DrawCalls, 0u);
				EXPECT_GT(Statistics.SplineMesh.Primitives, 0u);
				Renderer.PrepareViewResources(Commands, {Scene.get(), View, Output, false, {}}, Peer);
				EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Ready);
				Preparation.RequestCancel();
				Renderer.PrepareViewResources(Commands, {Scene.get(), View, Output, false, {}}, Peer);
				EXPECT_EQ(Preparation.GetProgress().RetainedObjects, 0u);
				EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Ready);
				const auto Drawn = GetMetalShaderCreationStatistics();
				EXPECT_EQ(Drawn.FunctionsCreated, Prepared.FunctionsCreated);
				EXPECT_EQ(Drawn.GraphicsPipelinesCreated, Prepared.GraphicsPipelinesCreated);
				EXPECT_EQ(Drawn.ComputePipelinesCreated, Prepared.ComputePipelinesCreated);
				EXPECT_EQ(Drawn.SourceCompileAttempts, Initial.SourceCompileAttempts);
				EXPECT_EQ(Preparation.GetProgress().UncoveredShaderAdmissions, 0u);
				EXPECT_EQ(Preparation.GetProgress().UncoveredPipelineAdmissions, 0u);
				GDynamicRHI->RHIEndFrame_RenderThread(Commands);
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				FByteBuffer PreparedPixels, DemandPixels;
				GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, PreparedPixels);
				Commands.PollTextureReadbacks(); Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
				EXPECT_EQ(Renderer.RenderView(Commands, {Scene.get(), View, Output, false, {}}), ERenderViewResult::Success);
				GDynamicRHI->RHIEndFrame_RenderThread(Commands);
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, DemandPixels);
				Commands.PollTextureReadbacks(); Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				EXPECT_FALSE(PreparedPixels.empty()); EXPECT_EQ(PreparedPixels, DemandPixels);
				EXPECT_EQ(GetMetalShaderCreationStatistics().FunctionsCreated, Drawn.FunctionsCreated);
				EXPECT_EQ(GetMetalShaderCreationStatistics().GraphicsPipelinesCreated, Drawn.GraphicsPipelinesCreated);
				Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			});
			FlushRenderingCommands();
			EnqueueRenderCommand("InvalidatePreparedShaderGeneration", [&](FRHICommandListImmediate& Commands) {
				AdvanceShaderReloadGeneration();
				EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Stale);
				Renderer.BeginFrame_RenderThread(Commands);
				EXPECT_EQ(Peer.GetProgress().RetainedObjects, 0u);
			});
			FlushRenderingCommands();
			Scene->Release(); Scene.reset(); FlushRenderingCommands();
			EXPECT_EQ(Peer.GetProgress().State, ERenderResourcePreparationState::Stale);
			EXPECT_EQ(Peer.GetProgress().RetainedObjects, 0u);
			EnqueueRenderCommand("ReleaseCookedSceneResources", [&](FRHICommandListImmediate&) { Geometry->ReleaseResources(); Material = nullptr; MaskedMaterial = nullptr; InstanceMaterial = nullptr; });
			FlushRenderingCommands(); Lifecycle.Shutdown(); FlushRenderingCommands();
			ShutdownRenderingThread(); FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None); RHIExit();
		}
		ShutdownShaderData();
	}
}
