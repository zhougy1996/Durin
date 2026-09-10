#include "Resources/EnvironmentLightingResources.h"

#include "Resources/RendererResourceCoordinator.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Rendering/ProceduralSkySceneProxy.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"
#include "Scene.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
    namespace
    {
        class FSkyLightingShader final : public FGlobalShader
        {
        public:
            DURIN_BEGIN_SHADER_PARAMETERS(FSkyLightingShader)
                DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(Source);
                DURIN_SHADER_PARAMETER_SAMPLER(SourceSampler);
                DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
                DURIN_SHADER_PARAMETER_GRAPH_STORAGE_IMAGE(Output);
            DURIN_END_SHADER_PARAMETERS();
            DURIN_DECLARE_GLOBAL_SHADER(FSkyLightingShader, FGlobalShader, "/Engine/SkyLighting", EShaderFrequency::Compute, "ComputeMain");
        };
        DURIN_IMPLEMENT_GLOBAL_SHADER(FSkyLightingShader);
        const FGlobalShaderSetRegistration GSkyLightingShaderSet("Renderer", "SkyLighting.Compute",
            EShaderRequestEligibility::GameAndEditor, {&FSkyLightingShader::StaticType()});

        struct FSkyLightingUniform
        {
            FProceduralSkyUniform Sky;
            std::array<uint32,4> Dispatch{};
            FVector4f Filter{0};
        };
        struct FPassParameters
        {
            FRDGTextureParameter Source, Output;
            static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
            {
                static const std::array Members{
                    MakeRDGShaderResourceParameterMemberMetadata<FPassParameters,FRDGTextureParameter,FRDGTextureParameter>(
                        "Source", offsetof(FPassParameters,Source), ERDGParameterMemberKind::Texture,
                        ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
                        ERDGUse::Read, ERHIAccess::ComputeShaderRead, ERHIBindingType::Texture),
                    MakeRDGShaderResourceParameterMemberMetadata<FPassParameters,FRDGTextureParameter,FRDGTextureParameter>(
                        "Output", offsetof(FPassParameters,Output), ERDGParameterMemberKind::Texture,
                        ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
                        ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, ERHIBindingType::StorageImage, nullptr, true)};
                static const auto Metadata = MakeInlineRDGParametersMetadata<FPassParameters>("SkyLightingPass", Members);
                return &Metadata;
            }
        };
        auto Now() -> double
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }
    struct FEnvironmentLightingResources::FState
    {
        FGlobalShaderSetRef ShaderSet;
        TShaderMapRef<FSkyLightingShader> Shader;
        FComputePipelineStateRHIRef Pipeline;
        FSamplerRHIRef Sampler;
        FTextureRHIRef Lut;
        FRenderResourceGeneration Generation;
    };
    FEnvironmentLightingResources::FEnvironmentLightingResources(FRendererResourceCoordinator& InCoordinator)
        : State(std::make_unique<FState>()), Coordinator(InCoordinator) {}
    FEnvironmentLightingResources::~FEnvironmentLightingResources() = default;

    auto FEnvironmentLightingResources::EnsureResources_RenderThread(FRHICommandListImmediate&) -> bool
    {
        CheckRenderingThread();
        const auto Generation=Coordinator.GetGeneration_RenderThread();
        if (State->Generation != Generation) { State=std::make_unique<FState>(); State->Generation=Generation; }
        if (State->Pipeline && State->Sampler) return true;
        if (!GDynamicRHI) return false;
        const auto* Caps=GDynamicRHI->RHIGetCapabilities();
        if (!Caps || !Caps->bSupportsSkyLighting || !Caps->bSupportsGPUTimestamps) return false;
        const std::array<const FGlobalShaderType*,1> Types{&FSkyLightingShader::StaticType()};
        State->ShaderSet=GetGlobalShaderMap().ResolveShaderSet("SkyLighting.Compute",Types,true,ReportRendererResourceCreateDiagnostic);
        if (!State->ShaderSet) return false;
        State->Shader=TShaderMapRef<FSkyLightingShader>(State->ShaderSet);
        auto* Shader=State->Shader.GetRHIShader(false);
        if (!Shader) return false;
        FComputePipelineStateInitializer Initializer;
        Initializer.ComputeShader=Shader;
        Initializer.PipelineLayout=State->ShaderSet.GetPipelineLayout();
        State->Pipeline=FRenderPipelineRequestScope::Compute("SkyLightingCompute",Initializer);
        State->Sampler=RHICreateSampler(FRHISamplerDesc::LinearClamp());
        return State->Pipeline && State->Sampler;
    }

    auto FEnvironmentLightingResources::UpdateScene_RenderThread(FRHICommandListImmediate& Commands,
        FScene& Scene, FRendererRDGAllocator& Allocator, FRHITexture* FallbackCube) -> void
    {
        CheckRenderingThread();
        const double CPUStart=Now();
        std::erase_if(Scenes, [](const auto& Entry) { return Entry.expired(); });
        if (std::none_of(Scenes.begin(), Scenes.end(), [&](const auto& Entry) { return Entry.lock()==Scene.SkyLighting; }))
            Scenes.emplace_back(Scene.SkyLighting);
        auto& S=*Scene.SkyLighting;
        const auto Light=Scene.GetSkyLight_RenderThread();
        const auto Sky=Scene.GetProceduralSky_RenderThread();
        const bool Captured=Light && Light->SourceMode==ESkyLightSourceMode::CapturedSky;
        FRHITexture* Source=Light && Light->Texture ? Light->Texture->GetReferencedTexture_RenderThread() : nullptr;
        const auto Generation=Coordinator.GetGeneration_RenderThread();
        const uint64 Provider=Captured && Sky ? Sky->InstanceId : 0;
        const auto HasAuthority=[&](const FSkyLightingGeneration& G) {
            return Light && (!Captured || Sky) && G.Owner==Light->InstanceId && G.SourceEpoch==Light->SourceEpoch
                && G.Provider==Provider && (Captured || G.Source.GetReference()==Source) && G.Resources==Generation;
        };
        if (S.Active && !HasAuthority(*S.Active))
        {
            S.Retiring.push_back(std::move(S.Active));
            S.LastAttempt=-60;
        }
        if (S.Query)
        {
            if (S.InFlight && S.InFlight->Resources.Device!=Generation.Device) { S.Query=nullptr; S.InFlight.reset(); }
            else
            {
                const auto Result=GDynamicRHI->RHIGetGPUTimingResult(S.Query);
                if (Result.State==ERHIGPUTimingResultState::Pending) return;
                if (Result.State==ERHIGPUTimingResultState::Ready && Light && S.InFlight && HasAuthority(*S.InFlight))
                    {
                    Light->UpdateStatus->UpdateMilliseconds.store(double(Result.DurationNanoseconds)/1.e6);
                    Light->UpdateStatus->CompletedUpdates.fetch_add(1, std::memory_order_release);
                }
                S.Query=nullptr;
                if (!S.InFlight || HasAuthority(*S.InFlight)) S.Retiring.clear();
                S.InFlight.reset();
            }
        }
        const auto MeasureImages=[&]() {
            std::erase_if(S.Generations,[](const auto& Entry) { return Entry.expired(); });
            std::unordered_set<FRHITexture*> Images;
            uint64 LightingBytes=0, SourceBytes=0;
            const auto Add=[&](FRHITexture* Texture,uint64& Bytes) {
                if (Texture && Images.insert(Texture).second) Bytes+=Texture->GetBackendAllocationBytes();
            };
            Add(State->Lut,LightingBytes);
            for (const auto& Entry:S.Generations) if (const auto G=Entry.lock())
            {
                Add(G->Radiance,LightingBytes); Add(G->Irradiance,LightingBytes); Add(G->Prefiltered,LightingBytes);
                Add(G->Source,SourceBytes);
            }
            if (Light) Light->UpdateStatus->RetainedImageBytes.store(LightingBytes);
            return std::pair(LightingBytes,SourceBytes);
        };
        if (!Light || (Captured && !Sky))
        {
            // A source removal still needs an ordered retirement marker after
            // the last view which could have sampled the removed generation.
            if (!S.Retiring.empty() && !S.Query)
            {
                S.Query=GDynamicRHI->RHICreateGPUTimingQuery();
                if (S.Query) { Commands.BeginGPUTimingQuery(S.Query); Commands.EndGPUTimingQuery(S.Query); }
            }
            return;
        }
        const double Time=Now();
        const bool Manual=!S.Active || S.Active->Request!=Light->RequestSerial;
        const bool Changed=Captured && S.Active && S.Active->Revision!=Sky->Revision;
        if (!Manual && !(Light->bAutomaticRefresh && Changed))
        {
            Light->UpdateStatus->SteadyCPUMilliseconds.store((Now()-CPUStart)*1000);
            return;
        }
        // Retry creation failures at the same bounded interval as animated updates.
        const bool NewRequest=S.AttemptOwner!=Light->InstanceId || S.AttemptEpoch!=Light->SourceEpoch
            || S.AttemptProvider!=Provider || S.AttemptRequest!=Light->RequestSerial;
        if (!NewRequest && Time-S.LastAttempt < Light->RefreshInterval)
        {
            Light->UpdateStatus->SteadyCPUMilliseconds.store((Now()-CPUStart)*1000);
            return;
        }
        S.LastAttempt=Time;
        S.AttemptOwner=Light->InstanceId; S.AttemptEpoch=Light->SourceEpoch;
        S.AttemptProvider=Provider; S.AttemptRequest=Light->RequestSerial;
        // Reserve a conservative 4 MiB including alignment for the fixed output
        // envelope before allocation. Count exports and consumers, not RDG pool entries.
        const auto [LightingBytes,SourceBytes]=MeasureImages();
        const bool SourceRetained=std::any_of(S.Generations.begin(),S.Generations.end(),[&](const auto& Entry) {
            const auto G=Entry.lock(); return G && G->Source.GetReference()==Source;
        });
        const uint64 NewSourceBytes=!Captured && Source && !SourceRetained ? Source->GetBackendAllocationBytes() : 0;
        if (LightingBytes+4ull*1024*1024>16ull*1024*1024
            || LightingBytes+SourceBytes+NewSourceBytes+4ull*1024*1024>64ull*1024*1024)
        {
            Light->UpdateStatus->State.store(ESkyLightUpdateState::Backpressure);
            if (!S.Active && !S.Retiring.empty())
            {
                S.Query=GDynamicRHI->RHICreateGPUTimingQuery();
                if(S.Query) { Commands.BeginGPUTimingQuery(S.Query); Commands.EndGPUTimingQuery(S.Query); }
            }
            return;
        }
        Light->UpdateStatus->State.store(ESkyLightUpdateState::Pending);
        if (!EnsureResources_RenderThread(Commands) || !FallbackCube)
        {
            const auto* Caps=GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
            Light->UpdateStatus->State.store(Caps && Caps->bSupportsSkyLighting && Caps->bSupportsGPUTimestamps
                ? ESkyLightUpdateState::Pending : ESkyLightUpdateState::Unsupported);
            return;
        }
        auto Candidate=std::make_shared<FSkyLightingGeneration>();
        Candidate->Resources=Generation; Candidate->Owner=Light->InstanceId;
        Candidate->SourceEpoch=Light->SourceEpoch; Candidate->Provider=Provider;
        Candidate->Revision=Captured ? Sky->Revision : 0; Candidate->Request=Light->RequestSerial;
        Candidate->Source=Captured ? nullptr : Source; Candidate->CaptureTime=Time;
        auto Query=GDynamicRHI->RHICreateGPUTimingQuery();
        if (!Query) { Light->UpdateStatus->State.store(ESkyLightUpdateState::Failed); return; }
        FRDGBuilder Graph;
        auto Input=Graph.RegisterExternalTexture(FTextureRHIRef(Captured ? FallbackCube : Source),"Sky.Source",
            ERHIAccess::GraphicsShaderRead,ERHIAccess::GraphicsShaderRead);
        uint32 SourceDimension=Captured ? FallbackCube->GetSizeX() : Source->GetSizeX();
        uint32 SourceMips=Captured ? FallbackCube->GetNumMips() : Source->GetNumMips();
        const auto CreateCube=[&](const char* Name,uint32 Dimension,uint32 Mips) {
            return Graph.CreateTexture({.Texture=FRHITextureCreateDesc::CreateCube(Name).SetExtent(Dimension)
                .SetNumMips(Mips).SetFormat(EPixelFormat::RGBA16_FLOAT)
                .SetFlags(ETextureCreateFlags::ShaderResource|ETextureCreateFlags::Storage|ETextureCreateFlags::SourceCopy|ETextureCreateFlags::CPUReadback)},Name);
        };
        const auto AddPass=[&](FRDGTextureHandle Output,uint32 Op,uint32 Face,uint32 Mip,uint32 Dimension,uint32 Samples,float Roughness) {
            auto P=Graph.AllocParameters<FPassParameters>();
            P->Source={Input,{ERHITextureAspect::Color,0,SourceMips,0,6}};
            P->Output={Output,{ERHITextureAspect::Color,Mip,1,Face,1}};
            FSkyLightingUniform Uniform;
            if (Captured) Uniform.Sky=MakeProceduralSkyUniform(Sky->Parameters);
            Uniform.Dispatch={Op,Face,Dimension,Samples};
            Uniform.Filter={Roughness,float(SourceDimension),float(SourceMips),0};
            Graph.AddPass(std::format("Sky.{}.Face{}.Mip{}",Op,Face,Mip),ERDGPassType::Compute,std::move(P),
                [this,Uniform,Dimension](FRHICommandListImmediate& Cmd,const FPassParameters& P,const FRDGParameterResolver& Resolver) {
                    const auto Buffer=Cmd.AllocateDynamicUniformBuffer(&Uniform,sizeof(Uniform));
                    const std::array Begin{FRHIBufferTransition{Buffer.Buffer,Buffer.Offset,Buffer.Size,ERHIAccess::Discard,ERHIAccess::ComputeUniformRead}};
                    Cmd.TransitionBuffers(Begin);
                    Cmd.SwitchPipeline(ERHIPipeline::Compute);
                    Cmd.SetComputePipelineState(*State->Pipeline);
                    FSkyLightingShader::FParameters Ordinary;
                    Ordinary.SourceSampler=State->Sampler; Ordinary.Params=Buffer;
                    const auto Bindings=Resolver.GetShaderParameters(P);
                    SetShaderParameters(Cmd,State->Shader,Bindings,Ordinary);
                    Cmd.Dispatch((Dimension+7)/8,(Dimension+7)/8,1);
                    const std::array End{FRHIBufferTransition{Buffer.Buffer,Buffer.Offset,Buffer.Size,ERHIAccess::ComputeUniformRead,ERHIAccess::GraphicsUniformRead}};
                    Cmd.TransitionBuffers(End);
                });
        };
        FTextureRHIRef Lut;
        if (!State->Lut)
        {
            const auto Output=Graph.CreateTexture({.Texture=FRHITextureCreateDesc::Create2D("Sky.BrdfLut",128,128,EPixelFormat::RGBA16_FLOAT)
                .SetFlags(ETextureCreateFlags::ShaderResource|ETextureCreateFlags::Storage|ETextureCreateFlags::SourceCopy|ETextureCreateFlags::CPUReadback)},"Sky.BrdfLut");
            AddPass(Output,3,0,0,128,1024,0);
            Graph.QueueTextureExtraction(Output,&Lut,ERHIAccess::GraphicsShaderRead);
        }
        if (Captured)
        {
            const auto Radiance=CreateCube("Sky.Radiance",128,8);
            for (uint32 Mip=0;Mip<8;++Mip) for(uint32 Face=0;Face<6;++Face)
                AddPass(Radiance,0,Face,Mip,std::max(128u>>Mip,1u),16,0);
            Graph.QueueTextureExtraction(Radiance,&Candidate->Radiance,ERHIAccess::GraphicsShaderRead);
            Input=Radiance; SourceDimension=128; SourceMips=8;
        }
        const auto Irradiance=CreateCube("Sky.Irradiance",16,1);
        const auto Prefiltered=CreateCube("Sky.Prefiltered",128,8);
        for(uint32 Face=0;Face<6;++Face) AddPass(Irradiance,1,Face,0,16,256,0);
        for(uint32 Mip=0;Mip<8;++Mip) for(uint32 Face=0;Face<6;++Face)
            AddPass(Prefiltered,2,Face,Mip,std::max(128u>>Mip,1u),128,float(Mip)/7);
        Graph.QueueTextureExtraction(Irradiance,&Candidate->Irradiance,ERHIAccess::GraphicsShaderRead);
        Graph.QueueTextureExtraction(Prefiltered,&Candidate->Prefiltered,ERHIAccess::GraphicsShaderRead);
        FRDGExecutionContext Context{Allocator};
        Commands.BeginGPUTimingQuery(Query);
        const auto Result=Graph.Execute(Commands,&Context);
        Commands.EndGPUTimingQuery(Query);
        Commands.SwitchPipeline(ERHIPipeline::Graphics);
        if (!Result.IsSuccess()) { Light->UpdateStatus->State.store(ESkyLightUpdateState::Failed); return; }
        if (Lut) State->Lut=std::move(Lut);
        // RDG guarantees every face and mip was recorded. Subsequent consumers use
        // the same ordered RHI timeline; this is not a CPU claim of GPU completion.
        if (!HasAuthority(*Candidate)) return;
        if (S.Active) S.Retiring.push_back(std::move(S.Active));
        S.Active=Candidate; S.InFlight=Candidate; S.Query=Query;
        S.Generations.push_back(Candidate);
        MeasureImages();
        Light->UpdateStatus->PublishedRevision.store(Candidate->Revision);
        Light->UpdateStatus->CaptureTime.store(Time);
        Light->UpdateStatus->State.store(ESkyLightUpdateState::Ready);
    }
    auto FEnvironmentLightingResources::SelectScene_RenderThread(const FScene* Scene) -> void
    { Selected=Scene ? Scene->SkyLighting->Active : nullptr; }
    auto FEnvironmentLightingResources::GetIrradiance_RenderThread() const -> FRHITexture*
    { const auto G=Selected.lock(); return G && G->Resources==Coordinator.GetGeneration_RenderThread() ? G->Irradiance.GetReference() : nullptr; }
    auto FEnvironmentLightingResources::GetPrefiltered_RenderThread() const -> FRHITexture*
    { const auto G=Selected.lock(); return G && G->Resources==Coordinator.GetGeneration_RenderThread() ? G->Prefiltered.GetReference() : nullptr; }
    auto FEnvironmentLightingResources::GetBrdfLut_RenderThread() const -> FRHITexture*
    { return State->Generation==Coordinator.GetGeneration_RenderThread() ? State->Lut.GetReference() : nullptr; }
    auto FEnvironmentLightingResources::GetSampler_RenderThread() const -> FRHISampler*
    { return State->Sampler.GetReference(); }
    auto FEnvironmentLightingResources::ReleaseResources_RenderThread() -> void
    {
        CheckRenderingThread();
        for (auto& Entry : Scenes)
            if (auto Scene = Entry.lock()) *Scene = {};
        Scenes.clear();
        State=std::make_unique<FState>(); Selected.reset();
    }
}
