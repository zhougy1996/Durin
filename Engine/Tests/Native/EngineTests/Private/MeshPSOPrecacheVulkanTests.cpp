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
    // Must be registered before its peer. A serial early-return collector loop
    // never reaches the peer and cannot satisfy this preparation dependency.
    class FWaitingProbeCollector final : public Durin::IPSOCollector
    {
        int32 Baseline = DeferredProbeCalls.load();
    public:
        FWaitingProbeCollector() : IPSOCollector(Durin::FPSOCollectorCreateManager::GetIndex(Durin::EShadingPath::Deferred, "WaitingProbe")) {}
        auto PreparePSOResources(const Durin::FMaterialPSOPrecacheParams&, const Durin::FRenderResourceGeneration&, bool Required)
            -> std::expected<Durin::EPSOCollectorPreparation, std::string> override
        {
            if (!Required) return Durin::EPSOCollectorPreparation::Skip;
            return DeferredProbeCalls.load() > Baseline ? Durin::EPSOCollectorPreparation::Ready : Durin::EPSOCollectorPreparation::Pending;
        }
        auto CollectPSOInitializers(const Durin::FSceneRenderPreparationConfig&, const Durin::FMaterialResource&,
            const Durin::FPSOPrecacheVertexFactoryData&, const Durin::FPSOPrecacheParams&, Durin::FPSOPrecacheDataArray&) -> void override {}
    };
    auto CreateWaitingProbe(Durin::ERHIFeatureLevel) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FWaitingProbeCollector>(); }
    auto CreateDeferredCollectionProbe(Durin::ERHIFeatureLevel) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FIndependentProbeCollector>(false); }
    auto CreateMobileCollectionProbe(Durin::ERHIFeatureLevel) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FIndependentProbeCollector>(true); }
    const Durin::FVertexFactoryType IndependentFactory("IndependentPrecacheOnly",
        Durin::EVertexFactoryFlags::SupportsPSOPrecaching, &Durin::FLocalVertexFactory::GetPSOPrecacheVertexFetchElements);
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
    ASSERT_TRUE(InitializeAssetRuntime()); ASSERT_TRUE(RefreshAssetRegistry());
    FModuleManager::Get().LoadModule("RenderCore");
    RHIInit(Tests::GetVulkanEngineTestInitializationContext()); ASSERT_NE(GDynamicRHI, nullptr);
    InitRenderingThread();
    FRendererModule Renderer; FModuleTestHarness Lifecycle("MeshPSOCollectionContract"); Lifecycle.Start(Renderer);
    struct FCleanup {
        FModuleTestHarness& Lifecycle;
        ~FCleanup() { Lifecycle.Shutdown(); ShutdownRenderingThread(); RHIExit(); }
    } Cleanup{Lifecycle};
	std::unique_ptr<FMeshPassProcessor> ReusableCollector;
	struct FReuseCleanup {
		std::unique_ptr<FMeshPassProcessor>& Collector;
		~FReuseCleanup() { EnqueueRenderCommand("ReleaseReusableCollector", [this](FRHICommandListImmediate&) { Collector.reset(); }); FlushRenderingCommands(); }
	} ReuseCleanup{ReusableCollector};
    DeferredProbeCalls = 0; MobileProbeCalls = 0;
    // Temporary contributions are added with no active material sessions and
    // removed after every session has retired. Neither has a draw-pass entry.
    FRegisterPSOCollectorCreateFunction WaitingProbe(&CreateWaitingProbe, EShadingPath::Deferred, "WaitingProbe");
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
            FMaterialPSOPrecacheParams RequestParams;
            FPSOPrecacheDataArray Initializers;
            bool Ready = false, Collected = false;
        };
        std::vector<FCase> Cases;
        FMaterialPSOPrecacheCollection Aggregate, IndependentAggregate;
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
            FMaterialPSOPrecacheCollection& IndependentAggregate;
            std::unique_ptr<FMeshPassProcessor>& ReusableCollector;
            ~FReleaseCases() {
                EnqueueRenderCommand("ReleaseMeshPSOCollectionCases", [this](FRHICommandListImmediate&) {
                    for (auto& Case : Cases) if (Case.Scenario == 18) ReusableCollector = std::move(Case.Processor);
                    Cases.clear(); Aggregate = {}; IndependentAggregate = {};
                });
                FlushRenderingCommands();
            }
        } ReleaseCases{Cases, Aggregate, IndependentAggregate, ReusableCollector};
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
                        Case.Processor = Case.Scenario == 18 && ReusableCollector ? std::move(ReusableCollector) : FPassProcessorManager::CreateMeshPassProcessor(Case.Pass);
                        auto& Params = Case.RequestParams;
                        Params = AggregateParams;
                        Params.Material.bTwoSided = Case.Material.GetStaticProperties().bTwoSided;
                        Params.PrecachePSOParams = Case.Params; Params.PreparationConfig = Case.Config;
                        Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&Case.Factory->GetType());
                    }
                    auto Params = Case.RequestParams;
                    if (Case.Scenario == 18 && Case.Collected) Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&LimitedFactories[0]->GetType());
                    const auto Ready = Case.Processor->PreparePSOResources(Params,
                        Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation, false);
                    if (!Ready) { Error = Ready.error(); return; }
                    if (*Ready == EPSOCollectorPreparation::Pending) { Complete = false; continue; }
                    if (*Ready == EPSOCollectorPreparation::Ready) {
                        if (Case.Scenario == 18 && Case.Collected) {
                            FPSOPrecacheDataArray NewFactory;
                            Case.Processor->CollectPSOInitializers(Case.Config, Case.Material, Params.VertexFactoryData, Case.Params, NewFactory);
                            ASSERT_FALSE(NewFactory.empty());
                            for (const auto& Data : NewFactory) EXPECT_EQ(Data.VertexFactoryType, &LimitedFactories[0]->GetType());
                        } else {
                            Case.Processor->CollectPSOInitializers(Case.Config, Case.Material, Params.VertexFactoryData, Case.Params, Case.Initializers);
                            FPSOPrecacheDataArray Again(1);
                            Case.Processor->CollectPSOInitializers(Case.Config, Case.Material, Params.VertexFactoryData, Case.Params, Again);
                            EXPECT_EQ(Again.size(), Case.Initializers.size());
                            if (Case.Scenario == 18) {
                                auto Changed = Case.Material;
                                auto Properties = Changed.GetStaticProperties(); Properties.bTwoSided = true; Changed.StaticProperties = Properties;
                                auto ChangedParams = Case.Params; ChangedParams.bReversedZ = !ChangedParams.bReversedZ;
                                FPSOPrecacheDataArray Current;
                                Case.Processor->CollectPSOInitializers(Case.Config, Changed, Params.VertexFactoryData, ChangedParams, Current);
                                ASSERT_EQ(Current.size() + 1, Case.Initializers.size());
                                for (const auto& Data : Current) EXPECT_EQ(Data.GetGraphicsInitializer().RasterizerState.CullMode, ERHICullMode::None);
                                EXPECT_EQ(std::ranges::count_if(Current, [&](const auto& Data) {
                                    return Data.bRequired && Data.GetGraphicsInitializer().DepthStencilState.CompareOp
                                        == (ChangedParams.bReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less);
                                }), 2);
                                Case.Collected = true; Complete = false; continue;
                            }
                        }
                    }
                    Case.Ready = true;
                }
                const auto Result = CollectMaterialPSOs(AggregateParams, Aggregate, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation, Aggregate.bRequiredPSOsCollected ? EPSOPrecacheCollectionPhase::Optional : EPSOPrecacheCollectionPhase::Required);
                if (!Result) { Error = Result.error(); return; }
                AggregateReady = *Result; Complete &= AggregateReady;
                auto IndependentParams = AggregateParams;
                IndependentParams.VertexFactoryData = FPSOPrecacheVertexFactoryData(&IndependentFactory);
                const auto Independent = CollectMaterialPSOs(IndependentParams, IndependentAggregate, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation, IndependentAggregate.bRequiredPSOsCollected ? EPSOPrecacheCollectionPhase::Optional : EPSOPrecacheCollectionPhase::Required);
                if (!Independent) { Error = Independent.error(); return; }
                Complete &= *Independent;
                ASSERT_EQ(IndependentAggregate.PSOInitializers.size(), *Independent ? 2u : 1u);
                for (const auto& Data : IndependentAggregate.PSOInitializers) {
                    EXPECT_EQ(Data.GetType(), FPSOPrecacheData::EType::Compute);
                    EXPECT_EQ(Data.PSOCollectorIndex, DeferredProbe.GetIndex());
                    EXPECT_EQ(Data.VertexFactoryType, nullptr);
                }

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
                EXPECT_EQ(Case.Initializers[Index].Priority, EPSOPrecachePriority::Medium);
                if (Case.Pass == EMeshPipelinePass::ShadowDepth) EXPECT_TRUE(Case.Initializers[Index].bRequired);
                if (Case.Pass == EMeshPipelinePass::HitProxy) EXPECT_FALSE(Case.Initializers[Index].bRequired);
            }
            for (uint8 Bits = 0; Bits < 8; ++Bits) {
                const FMeshPipelineRasterOptions Raster{(Bits & 4) != 0, (Bits & 1) != 0,
                    Case.Params.bOrientationChanging, (Bits & 2) != 0};
                if (Raster.bWireframe && !Case.Config.bSupportsWireframe) continue;
                const auto Draw = Case.Processor->DescribeMeshDraw(MakeMeshPipelineState(Identity, Raster), Case.Params.Topology, Blend, Raster.bReversedZ);
                auto Matches = [&](const FPSOPrecacheData& Data) {
                    const auto& PSO = Data.GetGraphicsInitializer();
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
                const auto Key = BuildGraphicsPipelineStateKey(Data.GetGraphicsInitializer(), GDynamicRHI->RHIGetCapabilities());
                ASSERT_TRUE(Key); Expected.emplace_back(*Key, Data.bRequired);
            }
        ASSERT_EQ(Aggregate.PSOInitializers.size(), Expected.size() + 2);
        size_t ComputeCount = 0;
        for (const auto& Data : Aggregate.PSOInitializers) {
            if (Data.GetType() == FPSOPrecacheData::EType::Compute) { EXPECT_TRUE(Data.IsValid()); ++ComputeCount; continue; }
            const auto Key = BuildGraphicsPipelineStateKey(Data.GetGraphicsInitializer(), GDynamicRHI->RHIGetCapabilities());
            ASSERT_TRUE(Key); EXPECT_EQ(Data.Priority, EPSOPrecachePriority::Medium);
            PSOPrecacheValidation::SetEnabled(true);
            auto Validation = PSOPrecacheValidation::Track(Data);
            ASSERT_TRUE(Validation);
            const auto Native = PipelineStateCache::PrecacheGraphicsPipelineState(Data.GetGraphicsInitializer(), "ValidationContract");
            ASSERT_TRUE(Native); ASSERT_TRUE((*Native)->Wait());
            PSOPrecacheValidation::Observe(Validation, (*Native)->GetStateObservation());
            EXPECT_EQ(PSOPrecacheValidation::Check(Data.GetGraphicsInitializer(), Data.PSOCollectorIndex, Data.VertexFactoryType).Result,
                PSOPrecacheValidation::EResult::Hit);
            auto DifferentState = Data.GetGraphicsInitializer();
            DifferentState.DepthStencilState.bEnableWrite = !DifferentState.DepthStencilState.bEnableWrite;
            EXPECT_NE(PSOPrecacheValidation::Check(DifferentState, Data.PSOCollectorIndex, Data.VertexFactoryType).Result,
                PSOPrecacheValidation::EResult::Hit);
            PSOPrecacheValidation::SetEnabled(false);
            const auto Found = std::ranges::find(Expected, std::pair{*Key, Data.bRequired});
            ASSERT_NE(Found, Expected.end()); Expected.erase(Found);
        }
        EXPECT_TRUE(Expected.empty());
        EXPECT_EQ(ComputeCount, 2u);

    }
    EXPECT_EQ(DeferredProbeCalls.load(), 12);
    EXPECT_EQ(MobileProbeCalls.load(), 0);

}
