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
        struct FMaterialPSOCollectionSession
        {
            struct FCollector
            {
                std::unique_ptr<IPSOCollector> Collector;
                FPSOPrecacheDataArray OptionalDescriptions;
                std::array<bool, 2> Complete{};
            };
            std::vector<FCollector> Collectors;
            FMaterialResource Material;

        };
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
        size_t PreparedGroup = 0;
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
            { return; }
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
    auto FMeshPSOCollectionSession::Prepare(EPSOPrecacheCollectionPhase Phase) -> std::expected<EPSOCollectorPreparation, std::string>
    {
        CheckRenderingThread();
        auto& S = *State;
        if (Phase != EPSOPrecacheCollectionPhase::Required && Phase != EPSOPrecacheCollectionPhase::Optional)
            return std::unexpected(std::string("Invalid PSO collection phase."));
        const size_t Group = Phase == EPSOPrecacheCollectionPhase::Required ? 0 : 1;
        if (S.Emitted[Group]) return EPSOCollectorPreparation::Skip;
        if (!S.Error.empty()) return std::unexpected(S.Error);
        if (Group == 1 && !S.Emitted[0]) return std::unexpected(std::string("Required PSO collection must finish before optional collection."));
        if (S.Variants[Group].empty()) { S.Emitted[Group] = true; return EPSOCollectorPreparation::Skip; }
        if (!S.ResourcesReady) {
            const auto Ready = S.PrepareResources();
            if (!Ready) { S.Error = Ready.error(); return std::unexpected(S.Error); }
            if (!*Ready) return EPSOCollectorPreparation::Pending;
        }
        S.PreparedGroup = Group;
        return EPSOCollectorPreparation::Ready;
    }
    auto FMeshPSOCollectionSession::AppendPrepared(FPSOPrecacheDataArray& Out) -> std::expected<void, std::string>
    {
        auto& S = *State;
        CheckRenderingThread();
        require(S.ResourcesReady && !S.Emitted[S.PreparedGroup]);
        const FPreparedMeshPSOCollectorResources Resources{&S.Shaders, S.Declaration.GetReference(), S.Params.Priority, S.Variants[S.PreparedGroup]};
        auto Result = S.Processor.AppendPreparedPSOInitializers(Resources, S.Params.PreparationConfig, S.Material,
            S.Params.VertexFactoryData, S.Params.PrecachePSOParams, Out);
        if (!Result) { S.Error = Result.error(); return Result; }
        S.Emitted[S.PreparedGroup] = true;
        return {};
    }
    auto FMeshPSOCollectionSession::CollectPSOInitializers(EPSOPrecacheCollectionPhase Phase, FPSOPrecacheDataArray& Out) -> std::expected<bool, std::string>
    {
        auto Ready = Prepare(Phase);
        if (!Ready) return std::unexpected(Ready.error());
        if (*Ready == EPSOCollectorPreparation::Pending) return false;
        if (*Ready == EPSOCollectorPreparation::Skip) return true;
        auto Result = AppendPrepared(Out);
        if (!Result) return std::unexpected(Result.error());
        return true;
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
                Resources->Collectors.push_back({std::move(Collector)});
            }
            Collection.Resources = std::move(Resources);
        }
        auto Resources = std::static_pointer_cast<FMaterialPSOCollectionSession>(Collection.Resources);
        for (size_t Group = 0; Group < 2; ++Group) {
            bool Complete = true;
            for (auto& Entry : Resources->Collectors) {
                if (Entry.Complete[Group]) continue;
                const auto Ready = Entry.Collector->PreparePSOResources(Params, Generation, Group == 0);
                if (!Ready) return std::unexpected(Ready.error());
                if (*Ready == EPSOCollectorPreparation::Pending) { Complete = false; continue; }
                if (*Ready == EPSOCollectorPreparation::Ready) {
                    FPSOPrecacheDataArray Initializers;
                    try {
                        Entry.Collector->CollectPSOInitializers(Params.PreparationConfig, Resources->Material,
                            Params.VertexFactoryData, Params.PrecachePSOParams, Initializers);
                    } catch (const std::exception& Error) { return std::unexpected(std::string(Error.what())); }
                    for (auto& Data : Initializers) {
                        Data.PSOCollectorIndex = Entry.Collector->PSOCollectorIndex;
                        Data.VertexFactoryType = Params.VertexFactoryData.VertexFactoryType;
                        if (Group == 0 && !Data.bRequired) Entry.OptionalDescriptions.push_back(std::move(Data));
                        else Collection.PSOInitializers.push_back(std::move(Data));
                    }
                }
                if (Group == 1) {
                    for (auto& Data : Entry.OptionalDescriptions) Collection.PSOInitializers.push_back(std::move(Data));
                    Entry.OptionalDescriptions.clear();
                }
                Entry.Complete[Group] = true;
            }
            if (!Complete) return false;
            if (Group == 0) Collection.bRequiredPSOsCollected = true;
        }
        return true;
    }
}
