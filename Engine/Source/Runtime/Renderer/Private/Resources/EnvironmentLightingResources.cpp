#include "Resources/EnvironmentLightingResources.h"
#include "RDG/RDG.h"

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
                    MakeRDGTextureReadMetadata<FPassParameters, FRDGTextureParameter, ERDGPassType::Compute>(
                        "Source", offsetof(FPassParameters, Source)),
                    MakeRDGComputeTextureWriteMetadata<FPassParameters, FRDGTextureParameter>(
                        "Output", offsetof(FPassParameters, Output))};
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
        if (!Caps || !Caps->bSupportsSkyLighting) return false;
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
        const auto RecordCompletion = [&]() {
            // All sky passes use the ordered graphics queue. This marker also
            // covers earlier views when a removed authority needs retirement.
            S.Completion = Commands.BeginGPUSubmission(
                {.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
            Commands.EndGPUSubmission();
        };
        if (S.Completion)
        {
            if (S.InFlight && S.InFlight->Resources.Device != Generation.Device)
            {
                S.Query = nullptr; S.Completion = nullptr; S.InFlight.reset();
            }
            else
            {
                const auto Completion = S.Completion.GetState();
                if (Completion == ERHIGPUSubmissionState::Pending
                    || Completion == ERHIGPUSubmissionState::Submitted) return;
                const auto Result = S.Query ? GDynamicRHI->RHIGetGPUTimingResult(S.Query) : FRHIGPUTimingResult{};
                if (Completion == ERHIGPUSubmissionState::Complete
                    && Result.State == ERHIGPUTimingResultState::Pending) return;
                if (Completion == ERHIGPUSubmissionState::Complete
                    && Light && S.InFlight && HasAuthority(*S.InFlight))
                {
                    if (Result.State == ERHIGPUTimingResultState::Ready)
                        Light->UpdateStatus->UpdateMilliseconds.store(double(Result.DurationNanoseconds)/1.e6);
                    Light->UpdateStatus->CompletedUpdates.fetch_add(1, std::memory_order_release);
                }
                S.Query = nullptr;
                if (S.Completion.IsRetirementEligible()
                    && (!S.InFlight || HasAuthority(*S.InFlight))) S.Retiring.clear();
                S.Completion = nullptr;
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
            if (!S.Retiring.empty() && !S.Completion)
            {
                RecordCompletion();
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
                RecordCompletion();
            }
            return;
        }
        Light->UpdateStatus->State.store(ESkyLightUpdateState::Pending);
        if (!EnsureResources_RenderThread(Commands) || !FallbackCube)
        {
            const auto* Caps=GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
            Light->UpdateStatus->State.store(Caps && Caps->bSupportsSkyLighting
                ? ESkyLightUpdateState::Pending : ESkyLightUpdateState::Unsupported);
            return;
        }
        auto Candidate=std::make_shared<FSkyLightingGeneration>();
        Candidate->Resources=Generation; Candidate->Owner=Light->InstanceId;
        Candidate->SourceEpoch=Light->SourceEpoch; Candidate->Provider=Provider;
        Candidate->Revision=Captured ? Sky->Revision : 0; Candidate->Request=Light->RequestSerial;
        Candidate->Source=Captured ? nullptr : Source; Candidate->CaptureTime=Time;
        const bool bTiming = GDynamicRHI->RHIGetCapabilities()->bSupportsGPUTimestamps;
        auto Query = bTiming ? GDynamicRHI->RHICreateGPUTimingQuery() : FGPUTimingQueryRHIRef{};
        if (bTiming && !Query) { Light->UpdateStatus->State.store(ESkyLightUpdateState::Failed); return; }
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
        const auto AddPass=[&](FRDGTextureHandle Output,uint32 Op,uint32 Mip,uint32 Dimension,uint32 Samples,float Roughness,uint32 Faces) {
            auto P=Graph.AllocParameters<FPassParameters>();
            P->Source={Input,{ERHITextureAspect::Color,0,SourceMips,0,6}};
            // Declare the entire mip, then bind one writable 2D face at a time.
            // Batching the faces avoids exhausting Metal's unsubmitted buffers.
            P->Output={Output,{ERHITextureAspect::Color,Mip,1,0,Faces}};
            FSkyLightingUniform Uniform;
            if (Captured) Uniform.Sky=MakeProceduralSkyUniform(Sky->Parameters);
            Uniform.Dispatch={Op,0,Dimension,Samples};
            Uniform.Filter={Roughness,float(SourceDimension),float(SourceMips),0};
            Graph.AddPass(std::format("Sky.{}.Mip{}",Op,Mip),ERDGPassType::Compute,std::move(P),
                [this,Uniform,Dimension,Faces,Mip](FRHICommandListImmediate& Cmd,const FPassParameters& P,const FRDGParameterResolver& Resolver) mutable {
                    Cmd.SwitchPipeline(ERHIPipeline::Compute);
                    Cmd.SetComputePipelineState(*State->Pipeline);
                    auto* Texture=Resolver.GetTexture(P.Output);
                    auto* SourceTexture=Resolver.GetTexture(P.Source);
                    auto SourceDesc=MakeDefaultTextureViewDesc(*SourceTexture,ERHITextureViewUsage::Sampled);
                    SourceDesc.Range=P.Source.Range;
                    auto SourceView=GDynamicRHI->RHIGetOrCreateTextureView(SourceTexture,SourceDesc);
                    requiref(SourceView,"Sky lighting source view is unavailable.");
                    const auto& Reflection=State->Shader.GetShader()->GetReflection();
                    const auto Bind=[&](std::string_view Name,FRHIResource* Resource,uint32 Offset=0,uint32 Size=0) {
                        const auto Binding=std::ranges::find_if(Reflection.ResourceBindings,
                            [Name](const auto& Candidate) { return Candidate.Name==Name; });
                        requiref(Binding!=Reflection.ResourceBindings.end(), "Sky lighting binding is missing.");
                        return FRHIShaderParameterResource{.Resource=Resource,.SetIndex=Binding->SetIndex,
                            .BindingIndex=Binding->BindingIndex,.Type=Binding->Type,.Offset=Offset,.Size=Size};
                    };
                    for(uint32 Face=0;Face<Faces;++Face)
                    {
                        auto Desc=MakeDefaultTextureViewDesc(*Texture,ERHITextureViewUsage::Storage);
                        Desc.Dimension=ERHITextureViewDimension::Texture2D;
                        Desc.Range={ERHITextureAspect::Color,Mip,1,Face,1};
                        auto View=GDynamicRHI->RHICreateTextureView(Texture,Desc);
                        requiref(View, "Sky lighting could not create a writable face view.");
                        Uniform.Dispatch[1]=Face;
                        const auto Buffer=Cmd.CreateUniformBufferRange(&Uniform,sizeof(Uniform));
                        // A complete update keeps prior bindings alive until replacement,
                        // including Vulkan's pending descriptor resource owners.
                        const std::array Parameters{
                            Bind("Source",SourceView.GetReference()),
                            Bind("SourceSampler",State->Sampler.GetReference()),
                            Bind("Params",Buffer.Buffer,Buffer.Offset,Buffer.Size),
                            Bind("Output",View.GetReference())};
                        Cmd.SetShaderParameters(State->Shader.GetRHIShader(),Parameters);
                        Cmd.Dispatch((Dimension+7)/8,(Dimension+7)/8,1);
                    }
                });
        };
        FTextureRHIRef Lut;
        if (!State->Lut)
        {
            const auto Output=Graph.CreateTexture({.Texture=FRHITextureCreateDesc::Create2D("Sky.BrdfLut",128,128,EPixelFormat::RGBA16_FLOAT)
                .SetFlags(ETextureCreateFlags::ShaderResource|ETextureCreateFlags::Storage|ETextureCreateFlags::SourceCopy|ETextureCreateFlags::CPUReadback)},"Sky.BrdfLut");
            AddPass(Output,3,0,128,1024,0,1);
            Graph.QueueTextureExtraction(Output,&Lut,ERHIAccess::GraphicsShaderRead);
        }
        if (Captured)
        {
            const auto Radiance=CreateCube("Sky.Radiance",128,8);
            for (uint32 Mip=0;Mip<8;++Mip)
                AddPass(Radiance,0,Mip,std::max(128u>>Mip,1u),16,0,6);
            Graph.QueueTextureExtraction(Radiance,&Candidate->Radiance,ERHIAccess::GraphicsShaderRead);
            Input=Radiance; SourceDimension=128; SourceMips=8;
        }
        const auto Irradiance=CreateCube("Sky.Irradiance",16,1);
        const auto Prefiltered=CreateCube("Sky.Prefiltered",128,8);
        AddPass(Irradiance,1,0,16,256,0,6);
        for(uint32 Mip=0;Mip<8;++Mip)
            AddPass(Prefiltered,2,Mip,std::max(128u>>Mip,1u),128,float(Mip)/7,6);
        Graph.QueueTextureExtraction(Irradiance,&Candidate->Irradiance,ERHIAccess::GraphicsShaderRead);
        Graph.QueueTextureExtraction(Prefiltered,&Candidate->Prefiltered,ERHIAccess::GraphicsShaderRead);

        if (Query) Commands.BeginGPUTimingQuery(Query);
        const auto Result=Graph.Execute(Commands,&Allocator);
        if (Query) Commands.EndGPUTimingQuery(Query);
        Commands.SwitchPipeline(ERHIPipeline::Graphics);
        if (!Result.has_value()) { Light->UpdateStatus->State.store(ESkyLightUpdateState::Failed); return; }
        RecordCompletion();
        // Submit this bounded job before admitting another scene's filtering.
        // This dispatches replay without waiting for GPU completion.
        Commands.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::SubmitToGPU);
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
