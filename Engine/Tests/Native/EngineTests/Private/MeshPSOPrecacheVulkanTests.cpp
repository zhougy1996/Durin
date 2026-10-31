#include "CoreMinimal.h"
#include "EngineTestSupport.h"
#include "PSOPrecacheMaterial.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialRenderProxy.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "AssetRegistry/Scan.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/MountPaths.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Rendering/SceneInterface.h"

#include <gtest/gtest.h>

TEST(FMeshPSOPrecacheVulkanTests, CollectedInitializersCoverDrawStatesParticipationAndRequiredRoutes)
{
    using namespace Durin; using namespace RendererPrivate;
    GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
    InitializeDObjectSystem();
    if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
    ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
    ASSERT_TRUE(InitializeAssetManager()); ASSERT_TRUE(RefreshAssetRegistry());
    FModuleManager::Get().LoadModule("RenderCore");
    RHIInit(Tests::GetVulkanEngineTestInitializationContext()); ASSERT_NE(GDynamicRHI, nullptr);
    InitRenderingThread();
    FRendererModule Renderer; FModuleTestHarness Lifecycle("MeshPSOCollectionContract"); Lifecycle.Start(Renderer);
    struct FCleanup {
        FModuleTestHarness& Lifecycle;
        ~FCleanup() { Lifecycle.Shutdown(); ShutdownRenderingThread(); RHIExit(); }
    } Cleanup{Lifecycle};
    const auto Factory = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey()); ASSERT_TRUE(Factory);
    const FPSOPrecacheVertexFactoryData VertexFactory(&Factory->GetType());
    const auto InitialConfig = Renderer.GetSceneRenderPreparationConfig(FSceneInterface::GetDefaultFeatureLevel());
    for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
    for (const auto Shading : {EMaterialShadingModel::Lit, EMaterialShadingModel::Unlit})
    {
        SCOPED_TRACE(std::format("blend={} shading={}", uint8(Blend), uint8(Shading)));
        TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
        FMaterialStaticProperties Properties; Properties.BlendMode = Blend; Properties.ShadingModel = Shading;
        ASSERT_TRUE(Material->SetStaticProperties(Properties));
        TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
        Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = FGuid::NewGuid(), .Name = "CollectorScalar"}; Parameter->DefaultValue = 0.5f;
        FMaterialExpressionSurfaceOutputs Outputs; Outputs.Roughness = {Parameter->Id};
        const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
        ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
        Material->CacheResourceShadersForRendering(InitialConfig.FeatureLevel);
        FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
        const auto Resource = Material->GetMaterialResource(InitialConfig.FeatureLevel);
        ASSERT_TRUE(Resource.HasValidGameThreadShaderMap());
        struct FCase {
            EMeshPipelinePass Pass;
            FSceneRenderPreparationConfig Config;
            FPSOPrecacheParams Params;
            FMaterialResource Material;
            std::unique_ptr<FMeshPassProcessor> Processor;
            FPSOPrecacheDataArray Initializers;
            bool Ready = false;
        };
        std::vector<FCase> Cases;
        struct FReleaseCases {
            std::vector<FCase>& Cases;
            ~FReleaseCases() {
                EnqueueRenderCommand("ReleaseMeshPSOCollectionCases", [this](FRHICommandListImmediate&) { Cases.clear(); });
                FlushRenderingCommands();
            }
        } ReleaseCases{Cases};
        // Full raster/topology matrix plus focused participation/configuration cases.
        for (uint8 Scenario = 0; Scenario < 15; ++Scenario)
        for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
        {
            FCase Case{Pass, InitialConfig, {}, Resource};
            auto P = Properties; P.bTwoSided = Scenario < 8 && (Scenario & 1); Case.Material.StaticProperties = P;
            Case.Params.bOrientationChanging = Scenario < 8 && (Scenario & 2);
            if (Scenario < 8 && (Scenario & 4)) Case.Params.Topology = EGeometryTopology::LineList;
            if (Scenario == 8) Case.Params.Participation.bCastShadow = false;
            if (Scenario == 9) Case.Params.Participation = {true, false};
            if (Scenario == 10) Case.Params.Participation = {false, false};
            if (Scenario == 11) {
                Case.Config.Settings.bHybrid = Case.Config.Settings.bHitProxy = Case.Config.bSupportsWireframe = false;
                Case.Params.bReversedZ = true;
            }
            if (Scenario == 12) Case.Config.Settings.bForward = false;
            if (Scenario == 13) { Case.Params.bWireframe = Case.Config.bSupportsWireframe; Case.Params.bReversedZ = true; }
            if (Scenario == 14) Case.Params.bWireframe = Case.Config.bSupportsWireframe;
            Case.Initializers.emplace_back(); // Caller-owned prefix must survive collection.
            Cases.push_back(std::move(Case));
        }
        bool Complete = false; std::string Error;
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        do {
            EnqueueRenderCommand("CollectMeshPSOContract", [&](FRHICommandListImmediate& Commands) {
                ++GRenderFrameCounterRenderThread; Renderer.BeginFrame_RenderThread(Commands);
                const auto DrawCount = Commands.GetNumRecordedDrawCommands();
                Complete = true;
                for (auto& Case : Cases) {
                    if (Case.Ready) continue;
                    if (!Case.Processor) Case.Processor = FPassProcessorManager::CreateMeshPassProcessor(Case.Pass,
                        {Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation, EPSOPrecachePriority::High});
                    IPSOCollector& Collector = *Case.Processor;
                    Collector.CollectPSOInitializers(Case.Config, Case.Material, VertexFactory, Case.Params, Case.Initializers);
                    const auto Result = Case.Processor->GetPSOCollectionResult();
                    if (!Result) { Error = Result.error(); return; }
                    Case.Ready = *Result; Complete &= Case.Ready;
                    if (Case.Ready) {
                        const auto Count = Case.Initializers.size();
                        Collector.CollectPSOInitializers(Case.Config, Case.Material, VertexFactory, Case.Params, Case.Initializers);
                        EXPECT_EQ(Case.Initializers.size(), Count);
                    }
                }
                EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawCount);
            });
            FlushRenderingCommands();
            if (!Error.empty() || Complete) break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < Deadline);
        ASSERT_TRUE(Error.empty()) << Error; ASSERT_TRUE(Complete);
        for (const auto& Case : Cases) {
            SCOPED_TRACE(std::format("pass={} topology={} main={} shadow={}", uint8(Case.Pass), uint8(Case.Params.Topology),
                Case.Params.Participation.bRenderInMainPass, Case.Params.Participation.bCastShadow));
            FMaterialPlanningPassIdentity Identity; Identity.ShaderMap.BlendMode = Blend; Identity.ShaderMap.ShadingModel = Shading;
            Identity.bTwoSided = Case.Material.GetStaticProperties().bTwoSided;
            const bool Main = Case.Params.Participation.bRenderInMainPass;
            bool Applicable = false;
            switch (Case.Pass) {
            case EMeshPipelinePass::Forward: Applicable = Main && Case.Config.Settings.bForward; break;
            case EMeshPipelinePass::HybridRetainedForward: Applicable = Main && Case.Config.Settings.bHybrid && Blend != EMaterialBlendMode::Translucent && Shading == EMaterialShadingModel::Unlit; break;
            case EMeshPipelinePass::SortedTranslucency: Applicable = Main && Case.Config.Settings.bHybrid && Blend == EMaterialBlendMode::Translucent; break;
            case EMeshPipelinePass::ShadowDepth: Applicable = Case.Params.Participation.bCastShadow && Blend != EMaterialBlendMode::Translucent; break;
            case EMeshPipelinePass::GBuffer: Applicable = Main && Case.Config.Settings.bHybrid && Blend != EMaterialBlendMode::Translucent && Shading == EMaterialShadingModel::Lit; break;
            case EMeshPipelinePass::HitProxy: Applicable = Main && Case.Config.Settings.bHitProxy; break;
            }
            ASSERT_FALSE(Case.Initializers.front().IsValid());
            if (!Applicable) { EXPECT_EQ(Case.Initializers.size(), 1u); continue; }
            ASSERT_GT(Case.Initializers.size(), 1u);
            const size_t ExpectedInitializers = Case.Pass == EMeshPipelinePass::ShadowDepth ? 2u
                : Case.Pass == EMeshPipelinePass::HitProxy ? 4u : Case.Config.bSupportsWireframe ? 8u : 4u;
            EXPECT_EQ(Case.Initializers.size() - 1, ExpectedInitializers);
            EXPECT_EQ(std::ranges::count_if(Case.Initializers, [](const auto& Data) { return Data.IsValid() && Data.bRequired; }),
                Case.Pass == EMeshPipelinePass::HitProxy ? 0 : 2);
            for (size_t Index = 1; Index < Case.Initializers.size(); ++Index) {
                EXPECT_TRUE(Case.Initializers[Index].IsValid());
                EXPECT_EQ(Case.Initializers[Index].Priority, EPSOPrecachePriority::High);
                if (Case.Pass == EMeshPipelinePass::ShadowDepth) EXPECT_TRUE(Case.Initializers[Index].bRequired);
                if (Case.Pass == EMeshPipelinePass::HitProxy) EXPECT_FALSE(Case.Initializers[Index].bRequired);
            }
            for (uint8 Bits = 0; Bits < 8; ++Bits) {
                const FMeshPipelineRasterOptions Raster{(Bits & 4) != 0, (Bits & 1) != 0,
                    Case.Params.bOrientationChanging, (Bits & 2) != 0};
                if (Raster.bWireframe && !Case.Config.bSupportsWireframe) continue;
                const auto Draw = Case.Processor->DescribeMeshDraw(MakeMeshPipelineState(Identity, Raster), Case.Params.Topology, Blend, Raster.bReversedZ);
                auto Matches = [&](const FPSOPrecacheData& Data) {
                    const auto& PSO = Data.GraphicsInitializer;
                    return Data.IsValid() && PSO.PrimitiveTopology == Draw.Topology && PSO.RenderTargetLayout == Draw.RenderTargets
                        && PSO.RasterizerState == Draw.Rasterizer && PSO.DepthStencilState == Draw.Depth && PSO.ColorBlendStates == Draw.ColorBlends;
                };
                EXPECT_TRUE(std::ranges::any_of(Case.Initializers, Matches));
                const bool Required = Case.Pass == EMeshPipelinePass::ShadowDepth || (Case.Pass != EMeshPipelinePass::HitProxy
                    && Raster.bWireframe == Case.Params.bWireframe && Raster.bReversedZ == Case.Params.bReversedZ);
                if (Required) EXPECT_TRUE(std::ranges::any_of(Case.Initializers, [&](const auto& Data) { return Data.bRequired && Matches(Data); }));
            }
        }
    }
}
