#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshRendererShared.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::RendererPrivate
{
    namespace
    {
        thread_local FPreparedMeshPSOCollectorResources* GPreparedMeshPSOResources = nullptr;
        struct FMaterialPSOCollectionSession
        {
            struct FCollector
            {
                std::unique_ptr<IPSOCollector> Collector;
                std::unique_ptr<FMeshPSOCollectionSession> Session;
                FPSOPrecacheDataArray OptionalDescriptions;
                bool ImmediateBuilt = false;
            };
            std::vector<FCollector> Collectors;
            FMaterialResource Material;
            std::array<size_t, 2> Next{};
        };
    }
    FPreparedMeshPSOCollectorScope::FPreparedMeshPSOCollectorScope(FPreparedMeshPSOCollectorResources& Resources)
        : Previous(std::exchange(GPreparedMeshPSOResources, &Resources))
    { CheckRenderingThread(); require(Resources.Shaders && Resources.Declaration); }
    FPreparedMeshPSOCollectorScope::~FPreparedMeshPSOCollectorScope() { GPreparedMeshPSOResources = Previous; }
    auto FPreparedMeshPSOCollectorScope::Get() -> FPreparedMeshPSOCollectorResources&
    {
        requiref(GPreparedMeshPSOResources, "Mesh PSO output requires prepared collection-session resources.");
        return *GPreparedMeshPSOResources;
    }
    static auto MakePSOCollectionMaterial(const FMaterialPSOPrecacheParams& Params) -> FMaterialResource
    {
        FMaterialResource Material;
        Material.CompiledProgram = Params.CompiledProgram;
        FMaterialStaticProperties Properties;
        Properties.BlendMode = static_cast<EMaterialBlendMode>(Params.Material.ShaderMap.BlendMode.Value);
        Properties.ShadingModel = static_cast<EMaterialShadingModel>(Params.Material.ShaderMap.ShadingModel.Value);
        Properties.OpacityMaskThreshold = Params.Material.ShaderMap.OpacityMaskThreshold;
        Properties.bTwoSided = Params.Material.bTwoSided;
        Properties.DepthWritePolicy = Params.Material.DepthWritePolicy;
        Material.StaticProperties = Properties;
        return Material;
    }
    struct FMeshPSOCollectionSession::FState
    {
        FMeshPassProcessor& Processor;
        FMaterialPSOPrecacheParams Params;
        FRenderResourceGeneration Generation;
        FMaterialResource Material;
        FMaterialPlanningPassIdentity Identity;
        std::shared_ptr<const FMeshVertexFactoryImplementation> Factory;
        std::array<std::vector<FMeshPSOPrecacheVariant>, 2> Variants;
        std::array<bool, 2> Emitted{};
        FShaderMapPreparation FixedShaders;
        FMeshPassShaders Shaders;
        FVertexDeclarationRHIRef Declaration;
        bool Requested = false, ResourcesReady = false;
        std::string Error;
        FState(FMeshPassProcessor& InProcessor, const FMaterialPSOPrecacheParams& InParams, const FRenderResourceGeneration& InGeneration)
            : Processor(InProcessor), Params(InParams), Generation(InGeneration)
        {
            CheckRenderingThread();
            Material = MakePSOCollectionMaterial(Params);
            const auto& Config = Params.PreparationConfig;
            if (!Params.CompiledProgram || !Params.CompiledProgram->ShaderMap || !Config.Version
                || Config.FeatureLevel != Params.FeatureLevel || Config.FeatureLevel != Params.CompiledProgram->FeatureLevel)
            { Error = "Mesh PSO session requires a matching accepted material and scene configuration."; return; }
            Factory = Params.VertexFactoryData.VertexFactoryType ? FindMeshVertexFactory(Params.VertexFactoryData.VertexFactoryType->GetStableKey()) : nullptr;
            if (!Factory || &Factory->GetType() != Params.VertexFactoryData.VertexFactoryType)
            { Error = "No registered mesh shader implementation for the precache Vertex Factory."; return; }
            const auto& Map = Params.CompiledProgram->GetShaderMap();
            Identity = Params.Material;
            Identity.ShaderMap.RenderLayout = Map.Layout.Identity;
            Identity.ShaderMap.ProgramIdentity = Map.ShaderMapId;
            if (!Processor.IsEnabled(Config) || !Processor.SupportsVertexFactory(*Factory)
                || !Processor.IsMeshRelevant(Identity, Params.PrecachePSOParams.Participation, Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return;
            for (const auto& Variant : Processor.GetPSOPrecacheVariants(Config, Params.PrecachePSOParams))
                Variants[Variant.bRequired ? 0 : 1].push_back(Variant);
        }
        auto PrepareResources() -> std::expected<bool, std::string>
        {
            if (!GDynamicRHI || !IsTaskSchedulerRunning()) return std::unexpected(std::string("Mesh PSO collection requires RHI and the task scheduler."));
            const auto Fragment = Processor.GetFragmentShader(Params.Material.ShaderMap.BlendMode);
            const auto MeshPass = Processor.GetVertexFactoryPass();
            if (!Fragment.Type || !Fragment.Resolve) return std::unexpected(std::string("Unsupported mesh pass fragment shader."));
            if (!Requested) {
                FixedShaders = FShaderMapPreparation::Request("MeshVertexFactory", {});
                if (!Factory->UsesGeneratedMaterialVertexStage(MeshPass)) FixedShaders.Add("MeshVertexFactory", {Factory->GetRuntimeRequestName(MeshPass)});
                if (Fragment.FixedModule) FixedShaders.Add(Fragment.FixedModule, {Fragment.FixedRequest});
                Requested = true;
            }
            const auto FixedReady = FixedShaders.Poll();
            if (!FixedReady || !*FixedReady) return FixedReady;
            FPreparedShaderMapScope Prepared;
            const auto& P = Params.PrecachePSOParams;
            const FMeshPassProcessorBatch Batch{Identity, *Factory, {}, P.Topology, P.Participation};
            const auto Completion = Processor.PrepareShaders(Batch, Generation, Params.CompiledProgram.get(), {}, Shaders);
            if (Completion.Status == EShaderBuildStatus::Pending) return false;
            if (!Completion) return std::unexpected(Completion.Error.empty() ? std::string("Mesh pass shader collection was canceled.") : std::string(Completion.Error));
            if (!Declaration) {
                const auto Elements = Params.VertexFactoryData.GetVertexElements();
                if (!Elements) return std::unexpected(std::string("Invalid precache vertex elements."));
                Declaration = GDynamicRHI->RHICreateVertexDeclaration(*Elements);
                if (!Declaration) return std::unexpected(std::string("Precache vertex declaration creation failed."));
            }
            ResourcesReady = true; return true;
        }
    };
    FMeshPSOCollectionSession::FMeshPSOCollectionSession(FMeshPassProcessor& Processor, const FMaterialPSOPrecacheParams& Params,
        const FRenderResourceGeneration& Generation) : State(std::make_unique<FState>(Processor, Params, Generation)) { CheckRenderingThread(); }
    FMeshPSOCollectionSession::~FMeshPSOCollectionSession() { CheckRenderingThread(); }
    auto FMeshPSOCollectionSession::CollectPSOInitializers(EPSOPrecacheCollectionPhase Phase, FPSOPrecacheDataArray& Out) -> std::expected<bool, std::string>
    {
        CheckRenderingThread();
        auto& S = *State;
        if (Phase != EPSOPrecacheCollectionPhase::Required && Phase != EPSOPrecacheCollectionPhase::Optional)
            return std::unexpected(std::string("Invalid PSO collection phase."));
        const size_t Group = Phase == EPSOPrecacheCollectionPhase::Required ? 0 : 1;
        if (S.Emitted[Group]) return true;
        if (!S.Error.empty()) return std::unexpected(S.Error);
        if (Group == 1 && !S.Emitted[0]) return std::unexpected(std::string("Required PSO collection must finish before optional collection."));
        if (S.Variants[Group].empty()) { S.Emitted[Group] = true; return true; }
        if (!S.ResourcesReady) {
            const auto Ready = S.PrepareResources();
            if (!Ready) { S.Error = Ready.error(); return std::unexpected(S.Error); }
            if (!*Ready) return false;
        }
        FPreparedMeshPSOCollectorResources Resources{&S.Shaders, S.Declaration.GetReference(), S.Params.Priority, S.Variants[Group]};
        FPreparedMeshPSOCollectorScope Scope(Resources);
        S.Processor.CollectPSOInitializers(S.Params.PreparationConfig, S.Material, S.Params.VertexFactoryData, S.Params.PrecachePSOParams, Out);
        if (!Resources.Error.empty()) { S.Error = std::move(Resources.Error); return std::unexpected(S.Error); }
        S.Emitted[Group] = true; return true;
    }
    auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params, FMaterialPSOPrecacheCollection& Collection,
        const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>
    {
        CheckRenderingThread();
        if (!Params.PreparationConfig.Version || Params.PreparationConfig.FeatureLevel != Params.FeatureLevel
            || !Params.CompiledProgram || Params.CompiledProgram->FeatureLevel != Params.FeatureLevel)
            return std::unexpected(std::string("Material PSO collection requires a matching Renderer snapshot and accepted program."));
        if (!Collection.Resources) {
            auto Resources = std::make_shared<FMaterialPSOCollectionSession>();
            Resources->Material = MakePSOCollectionMaterial(Params);
            // Both existing desktop paths use Deferred, independently of FeatureLevel.
            const auto Path = EShadingPath::Deferred;
            for (int32 Index = 0; Index < FPSOCollectorCreateManager::GetPSOCollectorCount(Path); ++Index) {
                const auto Create = FPSOCollectorCreateManager::GetCreateFunction(Path, Index);
                if (!Create) continue;
                auto Collector = Create(Params.FeatureLevel);
                if (!Collector || Collector->PSOCollectorIndex != Index)
                    return std::unexpected(std::string("Registered PSO factory returned an incompatible collector."));
                std::unique_ptr<FMeshPSOCollectionSession> Session;
                if (auto* MeshProcessor = dynamic_cast<FMeshPassProcessor*>(Collector.get()))
                    Session = std::make_unique<FMeshPSOCollectionSession>(*MeshProcessor, Params, Generation);
                Resources->Collectors.push_back({std::move(Collector), std::move(Session)});
            }
            Collection.Resources = std::move(Resources);
        }
        auto Resources = std::static_pointer_cast<FMaterialPSOCollectionSession>(Collection.Resources);
        for (size_t Group = 0; Group < 2; ++Group) {
            if (Group == 1) Collection.bRequiredPSOsCollected = true;
            while (Resources->Next[Group] < Resources->Collectors.size()) {
                auto& Entry = Resources->Collectors[Resources->Next[Group]];
                if (Entry.Session) {
                    const auto Result = Entry.Session->CollectPSOInitializers(
                        Group == 0 ? EPSOPrecacheCollectionPhase::Required : EPSOPrecacheCollectionPhase::Optional, Collection.PSOInitializers);
                    if (!Result || !*Result) return Result;
                } else {
                    // Ready non-mesh collectors append once; optional descriptions
                    // stay buffered until the global required boundary.
                    if (!Entry.ImmediateBuilt) {
                        FPSOPrecacheDataArray Initializers;
                        Entry.Collector->CollectPSOInitializers(Params.PreparationConfig, Resources->Material,
                            Params.VertexFactoryData, Params.PrecachePSOParams, Initializers);
                        for (auto& Data : Initializers) {
                            if (Data.bRequired) Collection.PSOInitializers.push_back(std::move(Data));
                            else Entry.OptionalDescriptions.push_back(std::move(Data));
                        }
                        Entry.ImmediateBuilt = true;
                    }
                    if (Group == 1) {
                        for (auto& Data : Entry.OptionalDescriptions) Collection.PSOInitializers.push_back(std::move(Data));
                        Entry.OptionalDescriptions.clear();
                    }
                }
                ++Resources->Next[Group];
            }
        }
        return true;
    }
}
