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
#include "Renderers/MeshRendererShared.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"
#include "Materials/MaterialShaderMap.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Rendering/SceneInterface.h"

#include <gtest/gtest.h>

namespace
{
    // Real registry contributions must precede shader-inventory freeze. These
    // reuse Local shader implementations and remove one capability each.
    class FLimitedPrecacheFactory final : public Durin::RendererPrivate::FMeshVertexFactoryImplementation
    {
        std::shared_ptr<const Durin::RendererPrivate::FMeshVertexFactoryImplementation> Base;
        Durin::FVertexFactoryType Type;
        uint32 Missing;
    public:
        FLimitedPrecacheFactory(std::string_view Name, uint32 InMissing)
            : Base(Durin::RendererPrivate::FindMeshVertexFactory(Durin::FStaticMeshBatchBinding{}.GetFactoryKey())),
              Type(Name, Base->GetType().GetFlags(), &Durin::FLocalVertexFactory::GetPSOPrecacheVertexFetchElements), Missing(InMissing) {}
        auto GetType() const -> const Durin::FVertexFactoryType& override { return Type; }
        auto GetLayoutKey() const -> Durin::FXxHash64 override { return Base->GetLayoutKey(); }
        auto GetShaderType(uint32 Pass) const -> Durin::FShaderType* override { return Pass == Missing ? nullptr : Base->GetShaderType(Pass); }
        auto UsesGeneratedMaterialVertexStage(uint32 Pass) const -> bool override { return Base->UsesGeneratedMaterialVertexStage(Pass); }
        auto Resolve(const Durin::FMeshMaterialShaderMap& Map, uint32 Pass) const
            -> std::shared_ptr<const Durin::RendererPrivate::FMeshVertexShaderBinding> override
        { return Pass == Missing ? nullptr : Base->Resolve(Map, Pass); }
    };
    class FOptionalFailureProcessor final : public Durin::RendererPrivate::FMeshPassProcessor
    {
        const FMeshPassProcessor& Base = Durin::RendererPrivate::FPassProcessorManager::GetMeshPassProcessor(Durin::RendererPrivate::EMeshPipelinePass::Forward);
    public:
        auto GetPass() const -> Durin::RendererPrivate::EMeshPipelinePass override { return Base.GetPass(); }
        auto AddMeshBatch(const Durin::RendererPrivate::FMeshPassProcessorBatch& Batch, const Durin::RendererPrivate::FMeshDrawCommandBuildInput& Input,
            Durin::RendererPrivate::FMeshDrawCommandBindingCache& Cache, Durin::RendererPrivate::FMeshPassDrawListContext& Context,
            std::shared_ptr<const Durin::RendererPrivate::FMeshDrawCommand> Existing) const -> Durin::FShaderCompletion override
        { return Base.AddMeshBatch(Batch, Input, Cache, Context, std::move(Existing)); }
        auto IsEnabled(const Durin::FSceneRenderPreparationConfig& Config) const -> bool override { return Base.IsEnabled(Config); }
        auto GetVertexFactoryPass(bool GPU = false) const -> uint32 override { return Base.GetVertexFactoryPass(GPU); }
        auto GetFragmentShader(Durin::FMaterialShaderBlendModeKey Blend) const -> Durin::RendererPrivate::FMeshPassFragmentShader override { return Base.GetFragmentShader(Blend); }
        auto DescribeMeshDraw(const Durin::RendererPrivate::FMeshPipelineState& State, Durin::FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
            Durin::FMaterialShaderBlendModeKey Blend, bool ReversedZ) const -> Durin::RendererPrivate::FMeshPipelineDescription override
        { return Base.DescribeMeshDraw(State, Topology, Blend, ReversedZ); }
        auto GetPSOPrecacheVariants(const Durin::FSceneRenderPreparationConfig& Config, const Durin::FPSOPrecacheParams& Params) const
            -> std::vector<Durin::RendererPrivate::FMeshPSOPrecacheVariant> override { return Base.GetPSOPrecacheVariants(Config, Params); }
        auto CollectPSOInitializers(const Durin::FSceneRenderPreparationConfig& Config, const Durin::FMaterialResource& Material,
            const Durin::FPSOPrecacheVertexFactoryData& Factory, const Durin::FPSOPrecacheParams& Params, Durin::FPSOPrecacheDataArray& Out) -> void override
        {
            using namespace Durin::RendererPrivate;
            auto& Outer = FPreparedMeshPSOCollectorScope::Get();
            if (!Outer.Variants.front().bRequired) {
                auto Nested = Outer;
                { FPreparedMeshPSOCollectorScope Scope(Nested); EXPECT_EQ(&FPreparedMeshPSOCollectorScope::Get(), &Nested); }
                EXPECT_EQ(&FPreparedMeshPSOCollectorScope::Get(), &Outer);
                Outer.Error = "Injected optional collection failure."; return;
            }
            FMeshPassProcessor::CollectPSOInitializers(Config, Material, Factory, Params, Out);
        }
    };
    std::atomic<int32> DeferredProbeCalls = 0, MobileProbeCalls = 0;
    class FIndependentProbeCollector final : public Durin::IPSOCollector
    {
        bool Mobile;
        Durin::FShaderRHIRef Shader = Durin::MakeRefCount<Durin::FRHIShader>(Durin::FRHIShaderDesc(Durin::EShaderFrequency::Compute, {}));
    public:
        explicit FIndependentProbeCollector(bool InMobile)
            : IPSOCollector(Durin::FPSOCollectorCreateManager::GetIndex(InMobile ? Durin::EShadingPath::Mobile : Durin::EShadingPath::Deferred,
                "IndependentCollectionProbe")), Mobile(InMobile) {}
        auto CollectPSOInitializers(const Durin::FSceneRenderPreparationConfig&, const Durin::FMaterialResource&,
            const Durin::FPSOPrecacheVertexFactoryData&, const Durin::FPSOPrecacheParams&, Durin::FPSOPrecacheDataArray& Out) -> void override
        {
            ++(Mobile ? MobileProbeCalls : DeferredProbeCalls);
            // Metadata-only compute descriptions exercise generic collector grouping;
            // this explicit collection fixture does not submit them to the driver.
            Durin::FComputePipelineStateInitializer Initializer; Initializer.ComputeShader = Shader.GetReference();
            Out.emplace_back(Initializer, false, Durin::EPSOPrecachePriority::High);
            Out.emplace_back(Initializer, true, Durin::EPSOPrecachePriority::High);
        }
    };
    auto CreateDeferredCollectionProbe(Durin::ERHIFeatureLevel) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FIndependentProbeCollector>(false); }
    auto CreateMobileCollectionProbe(Durin::ERHIFeatureLevel) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FIndependentProbeCollector>(true); }
    const auto LimitedFactories = [] {
        using namespace Durin; using namespace RendererPrivate;
        std::array<std::shared_ptr<const FMeshVertexFactoryImplementation>, 3> Factories{
            std::make_shared<FLimitedPrecacheFactory>("PrecacheWithoutShadow", MaterialMeshPassShadow),
            std::make_shared<FLimitedPrecacheFactory>("PrecacheWithoutForward", MaterialMeshPassForward),
            std::make_shared<FLimitedPrecacheFactory>("PrecacheWithoutGBuffer", MaterialMeshPassGBuffer)};
        for (const auto& Factory : Factories) require(RegisterMeshVertexFactory(Factory));
        return Factories;
    }();
}

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
    DeferredProbeCalls = 0; MobileProbeCalls = 0;
    // Temporary contributions are added with no active material sessions and
    // removed after every session has retired. Neither has a draw-pass entry.
    FRegisterPSOCollectorCreateFunction DeferredProbe(&CreateDeferredCollectionProbe, EShadingPath::Deferred, "IndependentCollectionProbe");
    FRegisterPSOCollectorCreateFunction MobileProbe(&CreateMobileCollectionProbe, EShadingPath::Mobile, "IndependentCollectionProbe");
    ASSERT_TRUE(DeferredProbe.IsRegistered() && MobileProbe.IsRegistered());
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
            std::shared_ptr<const FMeshVertexFactoryImplementation> Factory;
            uint8 Scenario;
            std::unique_ptr<FMeshPassProcessor> Processor;
            std::unique_ptr<FMeshPSOCollectionSession> Session;
            FPSOPrecacheDataArray Initializers;
            bool Ready = false;
        };
        std::vector<FCase> Cases;
        FMaterialPSOPrecacheCollection Aggregate;
        FMaterialPSOPrecacheParams AggregateParams;
        const auto& Map = Resource.CompiledProgram->GetShaderMap();
        AggregateParams.Material = {.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
            .BlendMode = Blend, .ShadingModel = Shading, .OpacityMaskThreshold = Properties.OpacityMaskThreshold},
            .bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
        AggregateParams.CompiledProgram = Resource.CompiledProgram; AggregateParams.VertexFactoryData = VertexFactory;
        AggregateParams.Priority = EPSOPrecachePriority::High; AggregateParams.FeatureLevel = InitialConfig.FeatureLevel;
        AggregateParams.PreparationConfig = InitialConfig;
        bool AggregateReady = false;
        struct FReleaseCases {
            std::vector<FCase>& Cases;
            FMaterialPSOPrecacheCollection& Aggregate;
            ~FReleaseCases() {
                EnqueueRenderCommand("ReleaseMeshPSOCollectionCases", [this](FRHICommandListImmediate&) { Cases.clear(); Aggregate = {}; });
                FlushRenderingCommands();
            }
        } ReleaseCases{Cases, Aggregate};
        // Full raster/topology matrix plus focused participation/configuration cases.
        for (uint8 Scenario = 0; Scenario < 19; ++Scenario)
        for (const auto Pass : FPassProcessorManager::GetRegisteredPasses())
        {
            if (Scenario == 18 && Pass != EMeshPipelinePass::Forward) continue;
            FCase Case{Pass, InitialConfig, {}, Resource, Scenario < 15 || Scenario == 18 ? Factory : LimitedFactories[Scenario - 15], Scenario};
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
                    if (!Case.Processor) {
                        if (Case.Scenario == 18) Case.Processor = std::make_unique<FOptionalFailureProcessor>();
                        else Case.Processor = FPassProcessorManager::CreateMeshPassProcessor(Case.Pass);
                        auto Params = AggregateParams;
                        Params.Material.bTwoSided = Case.Material.GetStaticProperties().bTwoSided;
                        Params.PrecachePSOParams = Case.Params; Params.PreparationConfig = Case.Config;
                        Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&Case.Factory->GetType());
                        Case.Session = std::make_unique<FMeshPSOCollectionSession>(*Case.Processor, Params, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation);
                        EXPECT_FALSE(Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, Case.Initializers));
                        EXPECT_EQ(Case.Initializers.size(), 1u);
                        if (Case.Scenario == 0 && Case.Pass == EMeshPipelinePass::Forward) {
                            auto Excluded = Params; Excluded.PrecachePSOParams.Participation = {false, false};
                            auto Peer = std::make_unique<FMeshPSOCollectionSession>(*Case.Processor, Excluded, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation);
                            FPSOPrecacheDataArray PeerOut(1);
                            ASSERT_TRUE(*Peer->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Required, PeerOut));
                            ASSERT_TRUE(*Peer->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, PeerOut));
                            EXPECT_EQ(PeerOut.size(), 1u);
                        }
                    }
                    auto Result = Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Required, Case.Initializers);
                    if (!Result) { Error = Result.error(); return; }
                    if (*Result && Case.Scenario == 18) {
                        const auto RequiredCount = Case.Initializers.size();
                        const auto Failed = Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, Case.Initializers);
                        EXPECT_FALSE(Failed.has_value());
                        if (!Failed) EXPECT_EQ(Failed.error(), "Injected optional collection failure.");
                        const auto RequiredAgain = Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Required, Case.Initializers);
                        EXPECT_TRUE(RequiredAgain.has_value());
                        if (RequiredAgain) EXPECT_TRUE(*RequiredAgain);
                        EXPECT_FALSE(Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, Case.Initializers));
                        EXPECT_EQ(Case.Initializers.size(), RequiredCount);
                        Case.Ready = true; continue;
                    }
                    if (*Result) Result = Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, Case.Initializers);
                    if (!Result) { Error = Result.error(); return; }
                    Case.Ready = *Result; Complete &= Case.Ready;
                    if (Case.Ready) {
                        const auto Count = Case.Initializers.size();
                        EXPECT_TRUE(*Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Required, Case.Initializers));
                        EXPECT_TRUE(*Case.Session->CollectPSOInitializers(EPSOPrecacheCollectionPhase::Optional, Case.Initializers));
                        EXPECT_EQ(Case.Initializers.size(), Count);
                    }
                }
                const auto Result = CollectMaterialPSOs(AggregateParams, Aggregate, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation);
                if (!Result) { Error = Result.error(); return; }
                AggregateReady = *Result; Complete &= AggregateReady;
                bool OptionalSeen = false;
                for (const auto& Data : Aggregate.PSOInitializers) {
                    OptionalSeen |= !Data.bRequired;
                    if (OptionalSeen) { EXPECT_FALSE(Data.bRequired); EXPECT_TRUE(Aggregate.bRequiredPSOsCollected); }
                }
                if (Aggregate.bRequiredPSOsCollected)
                    EXPECT_EQ(std::ranges::count_if(Aggregate.PSOInitializers, [](const auto& Data) { return Data.bRequired; }),
                        Blend == EMaterialBlendMode::Translucent ? 5 : 7);
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
            SCOPED_TRACE(Case.Factory->GetType().GetName());
            if (Case.Scenario == 18) {
                ASSERT_EQ(Case.Initializers.size(), 3u);
                EXPECT_TRUE(Case.Initializers[1].bRequired && Case.Initializers[2].bRequired);
                continue;
            }
            FMaterialPlanningPassIdentity Identity; Identity.ShaderMap.BlendMode = Blend; Identity.ShaderMap.ShadingModel = Shading;
            Identity.bTwoSided = Case.Material.GetStaticProperties().bTwoSided;
            const bool Main = Case.Params.Participation.bRenderInMainPass && Case.Factory->GetShaderType(MaterialMeshPassForward);
            const bool HasGBuffer = Case.Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr;
            bool Applicable = false;
            switch (Case.Pass) {
            case EMeshPipelinePass::Forward: Applicable = Main && Case.Config.Settings.bForward; break;
            case EMeshPipelinePass::HybridRetainedForward: Applicable = Main && Case.Config.Settings.bHybrid && Blend != EMaterialBlendMode::Translucent && (Shading == EMaterialShadingModel::Unlit || !HasGBuffer); break;
            case EMeshPipelinePass::SortedTranslucency: Applicable = Main && Case.Config.Settings.bHybrid && Blend == EMaterialBlendMode::Translucent; break;
            case EMeshPipelinePass::ShadowDepth: Applicable = Case.Params.Participation.bCastShadow && Blend != EMaterialBlendMode::Translucent && Case.Factory->GetShaderType(MaterialMeshPassShadow); break;
            case EMeshPipelinePass::GBuffer: Applicable = Main && Case.Config.Settings.bHybrid && Blend != EMaterialBlendMode::Translucent && Shading == EMaterialShadingModel::Lit && HasGBuffer; break;
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
        ASSERT_TRUE(AggregateReady); ASSERT_TRUE(Aggregate.bRequiredPSOsCollected);
        std::vector<std::pair<FGraphicsPipelineStateKey, bool>> Expected;
        for (const auto& Case : Cases) if (Case.Scenario == 0)
            for (size_t Index = 1; Index < Case.Initializers.size(); ++Index) {
                const auto& Data = Case.Initializers[Index];
                const auto Key = BuildGraphicsPipelineStateKey(Data.GraphicsInitializer, GDynamicRHI->RHIGetCapabilities());
                ASSERT_TRUE(Key); Expected.emplace_back(*Key, Data.bRequired);
            }
        ASSERT_EQ(Aggregate.PSOInitializers.size(), Expected.size() + 2);
        size_t ComputeCount = 0;
        for (const auto& Data : Aggregate.PSOInitializers) {
            if (Data.Type == FPSOPrecacheData::EType::Compute) { EXPECT_TRUE(Data.IsValid()); ++ComputeCount; continue; }
            const auto Key = BuildGraphicsPipelineStateKey(Data.GraphicsInitializer, GDynamicRHI->RHIGetCapabilities());
            ASSERT_TRUE(Key); EXPECT_EQ(Data.Priority, EPSOPrecachePriority::High);
            const auto Found = std::ranges::find(Expected, std::pair{*Key, Data.bRequired});
            ASSERT_NE(Found, Expected.end()); Expected.erase(Found);
        }
        EXPECT_TRUE(Expected.empty());
        EXPECT_EQ(ComputeCount, 2u);

    }
    EXPECT_EQ(DeferredProbeCalls.load(), 6);
    EXPECT_EQ(MobileProbeCalls.load(), 0);

}
