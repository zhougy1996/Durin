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
                bool Collected = false;
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
    struct FMeshPSOPrecacheResources::FState
    {
        FMeshMaterialShaderMapIdentity Identity;
        std::shared_ptr<const FMaterialCompilerResult> Program;
        FPSOPrecacheVertexFactoryData VertexFactoryData;
        FRenderResourceGeneration Generation;
        FShaderMapPreparation FixedShaders;
        FMeshPassShaders Shaders;
        FVertexDeclarationRHIRef Declaration;
        bool Requested = false, Ready = false;
    };
    FMeshPSOPrecacheResources::FMeshPSOPrecacheResources() = default;
    FMeshPSOPrecacheResources::~FMeshPSOPrecacheResources() { CheckRenderingThread(); }
    auto FMeshPSOPrecacheResources::Prepare(const FMeshPassProcessor& Processor, const FMaterialPSOPrecacheParams& Params,
        const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>
    {
        CheckRenderingThread();
        if (!Params.CompiledProgram || !Params.CompiledProgram->ShaderMap
            || Params.CompiledProgram->FeatureLevel != Params.FeatureLevel || Params.PreparationConfig.FeatureLevel != Params.FeatureLevel)
            return std::unexpected(std::string("Mesh PSO preparation requires a matching accepted program."));
        if (!State || State->Identity != Params.Material.ShaderMap || State->Program != Params.CompiledProgram
            || State->VertexFactoryData != Params.VertexFactoryData || State->Generation != Generation) {
            State = std::make_unique<FState>();
            State->Identity = Params.Material.ShaderMap; State->Program = Params.CompiledProgram;
            State->VertexFactoryData = Params.VertexFactoryData; State->Generation = Generation;
        }
        auto& S = *State;
        if (S.Ready) return true;
        if (!GDynamicRHI || !IsTaskSchedulerRunning()) return std::unexpected(std::string("Mesh PSO collection requires RHI and the task scheduler."));
        const auto Factory = FindMeshVertexFactory(Params.VertexFactoryData.VertexFactoryType->GetStableKey());
        if (!Factory) return std::unexpected(std::string("No mesh implementation for the prepared Vertex Factory."));
        const auto Fragment = Processor.GetFragmentShader(Params.Material.ShaderMap.BlendMode);
        const auto MeshPass = Processor.GetVertexFactoryPass();
        if (!Fragment.Type || !Fragment.Resolve) return std::unexpected(std::string("Unsupported mesh pass fragment shader."));
        if (!S.Requested) {
            S.FixedShaders = FShaderMapPreparation::Request("MeshVertexFactory", {});
            if (!Factory->UsesGeneratedMaterialVertexStage(MeshPass)) S.FixedShaders.Add("MeshVertexFactory", {Factory->GetRuntimeRequestName(MeshPass)});
            if (Fragment.FixedModule) S.FixedShaders.Add(Fragment.FixedModule, {Fragment.FixedRequest});
            S.Requested = true;
        }
        const auto FixedReady = S.FixedShaders.Poll();
        if (!FixedReady || !*FixedReady) return FixedReady;
        FPreparedShaderMapScope Prepared;
        const auto& P = Params.PrecachePSOParams;
        const FMeshPassProcessorBatch Batch{Params.Material, *Factory, {}, P.Topology, P.Participation};
        const auto Completion = Processor.PrepareShaders(Batch, Generation, Params.CompiledProgram.get(), {}, S.Shaders);
        if (Completion.Status == EShaderBuildStatus::Pending) return false;
        if (!Completion) return std::unexpected(Completion.Error.empty() ? std::string("Mesh shader preparation was canceled.") : std::string(Completion.Error));
        const auto Elements = Params.VertexFactoryData.GetVertexElements();
        if (!Elements) return std::unexpected(std::string("Invalid precache vertex elements."));
        S.Declaration = GDynamicRHI->RHICreateVertexDeclaration(*Elements);
        if (!S.Declaration) return std::unexpected(std::string("Precache vertex declaration creation failed."));
        S.Ready = true; return true;
    }
    auto FMeshPSOPrecacheResources::GetShaders() const -> const FMeshPassShaders&
    { require(State && State->Ready); return State->Shaders; }
    auto FMeshPSOPrecacheResources::GetDeclaration() const -> FRHIVertexDeclaration*
    { require(State && State->Ready); return State->Declaration; }
    auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params, FMaterialPSOPrecacheCollection& Collection,
        const FRenderResourceGeneration& Generation, EPSOPrecacheCollectionPhase Phase) -> std::expected<bool, std::string>
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
        const size_t Group = Phase == EPSOPrecacheCollectionPhase::Required ? 0 : 1;
        bool Complete = true;
        for (auto& Entry : Resources->Collectors) {
            if (!Entry.Collected) {
                const auto Ready = Entry.Collector->PreparePSOResources(Params, Generation, Group == 0);
                if (!Ready) return std::unexpected(Ready.error());
                if (*Ready == EPSOCollectorPreparation::Pending) { Complete = false; continue; }
                if (*Ready == EPSOCollectorPreparation::Ready) {
                    FPSOPrecacheDataArray Initializers;
                    Entry.Collector->CollectPSOInitializers(Params.PreparationConfig, Resources->Material,
                        Params.VertexFactoryData, Params.PrecachePSOParams, Initializers);
                    for (auto& Data : Initializers) {
                        Data.PSOCollectorIndex = Entry.Collector->PSOCollectorIndex;
                        if (!Data.VertexFactoryType && Data.GetType() == FPSOPrecacheData::EType::Graphics)
                            Data.VertexFactoryType = Params.VertexFactoryData.VertexFactoryType;
                        if (!Data.bRequired) Entry.OptionalDescriptions.push_back(std::move(Data));
                        else Collection.PSOInitializers.push_back(std::move(Data));
                    }
                    Entry.Collected = true;
                }
            }
            if (Group == 1) {
                for (auto& Data : Entry.OptionalDescriptions) Collection.PSOInitializers.push_back(std::move(Data));
                Entry.OptionalDescriptions.clear();
            }
        }
        if (!Complete) return false;
        if (Group == 0) Collection.bRequiredPSOsCollected = true;
        return Phase == EPSOPrecacheCollectionPhase::Optional
            || std::ranges::all_of(Resources->Collectors, [](const auto& Entry) { return Entry.Collected && Entry.OptionalDescriptions.empty(); });
    }
}
