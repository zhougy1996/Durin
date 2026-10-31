#include "Resources/EnvironmentLightingResources.h"
#include "RDG/RDG.h"

#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RendererResourceRetirement.h"
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
        FRenderPipelineRequests Requests;
    };
    FEnvironmentLightingResources::FEnvironmentLightingResources(FRendererResourceCoordinator& InCoordinator, FRendererResourceRetirementQueue& InRetirement)
        : Retirement(InRetirement), State(std::make_unique<FState>()), Coordinator(InCoordinator) {}
    FEnvironmentLightingResources::~FEnvironmentLightingResources() = default;

    auto FSkyLightingCaptureSchedule::CanAttempt(const FSkyLightingRequestKey& Request, double Time,
        bool bExplicitCapture, double RefreshInterval) const -> bool
    {
        if (FailedRequest == Request && Time < RetryAfter) return false;
        return bExplicitCapture || Time - LastSubmission >= RefreshInterval;
    }

    auto FSkyLightingCaptureSchedule::Fail(const FSkyLightingRequestKey& Request, double Time, double RetryInterval) -> void
    {
        FailedRequest = Request;
        RetryAfter = Time + RetryInterval;
    }

    auto FSkyLightingCaptureSchedule::Submit(double Time) -> void
    {
        LastSubmission = Time;
        FailedRequest.reset();
    }

    auto FSkyLightingSceneState::PollJob(bool bCurrentAuthority,
        const std::shared_ptr<FSkyLightUpdateStatus>& Status, double Milliseconds)
        -> std::optional<FSkyLightingJob>
    {
        if (!Job) return {};
        const auto Completion = Job->Completion.GetState();
        if (Completion == ERHIGPUSubmissionState::Pending || Completion == ERHIGPUSubmissionState::Submitted)
            return {};
        if (bCurrentAuthority)
        {
            if (Completion == ERHIGPUSubmissionState::Complete)
            {
                Published = Job->Output;
                if (Status)
                {
                    Status->PublishedRevision = Published->Revision;
                    Status->CaptureTime = Published->CaptureTime;
                    Status->UpdateMilliseconds = Milliseconds;
                    Status->CompletedUpdates.fetch_add(1, std::memory_order_release);
                    Status->State = ESkyLightUpdateState::Ready;
                }
            }
            else if (Status) Status->State = ESkyLightUpdateState::Failed;
        }
        return std::exchange(Job, std::nullopt);
    }

    auto FEnvironmentLightingResources::PrepareResources_RenderThread() -> ERenderResourceAvailability
    {
        CheckRenderingThread();
        const auto Generation=Coordinator.GetGeneration_RenderThread();
        if (State->Generation != Generation) { State=std::make_unique<FState>(); State->Generation=Generation; }
        if (State->Pipeline && State->Sampler) return ERenderResourceAvailability::Ready;
        if (!GDynamicRHI) return ERenderResourceAvailability::Failed;
        FRenderPipelineRequestScope Requests(State->Requests, Generation);
        const std::array<const FGlobalShaderType*,1> Types{&FSkyLightingShader::StaticType()};
        const auto Completion=GetGlobalShaderMap().ResolveShaderSet("SkyLighting.Compute",Types,true,ReportRendererResourceCreateDiagnostic);
        State->ShaderSet=Completion.Value.value_or(FGlobalShaderSetRef{});
        if (!State->ShaderSet)
            return Completion.Status == EShaderBuildStatus::Pending ? ERenderResourceAvailability::Pending : ERenderResourceAvailability::Failed;
        State->Shader=TShaderMapRef<FSkyLightingShader>(State->ShaderSet);
        auto* Shader=State->Shader.GetRHIShader(false);
        if (!Shader)
            return Requests.HasPending() ? ERenderResourceAvailability::Pending : ERenderResourceAvailability::Failed;
        FComputePipelineStateInitializer Initializer;
        Initializer.ComputeShader=Shader;
        Initializer.PipelineLayout=State->ShaderSet.GetPipelineLayout();
        State->Pipeline=FRenderPipelineRequestScope::Compute("SkyLightingCompute",Initializer);
        if (!State->Pipeline)
            return Requests.HasPending() ? ERenderResourceAvailability::Pending : ERenderResourceAvailability::Failed;
        State->Sampler=RHICreateSampler(FRHISamplerDesc::LinearClamp());
        return State->Sampler ? ERenderResourceAvailability::Ready : ERenderResourceAvailability::Failed;
    }

    auto FEnvironmentLightingResources::RetirePublished_RenderThread(FRHICommandListImmediate& Commands,
        std::shared_ptr<const FSkyLightingGeneration> Generation) -> void
    {
        if (!Generation) return;
        // Cover every prior consumer, including views recorded after the filter job.
        auto Completion = Commands.BeginGPUSubmission({.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
        Commands.EndGPUSubmission();
        const auto Device = Generation->RHIDeviceGeneration;
        Retirement.Retire(std::move(Generation), std::move(Completion), Device);
        Commands.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::SubmitToGPU);
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
        S.bUpdatePending = false;
        const auto Light=Scene.GetSkyLight_RenderThread();
        const auto PendingLight=Scene.GetSkyLight_RenderThread(false);
        const auto Sky=Scene.GetProceduralSky_RenderThread();
        const bool Captured=Light && Light->SourceMode==ESkyLightSourceMode::CapturedSky;
        FRHITexture* Source=Light && Light->Texture ? Light->Texture->GetReferencedTexture_RenderThread() : nullptr;
        const auto Generation=Coordinator.GetGeneration_RenderThread();
        const uint64 Provider=Captured && Sky ? Sky->InstanceId : 0;
        const auto HasAuthority=[&](const FSkyLightingGeneration& G) {
            return Light && (!Captured || Sky) && G.Owner==Light->InstanceId && G.SourceEpoch==Light->SourceEpoch
                && G.Provider==Provider && (Captured || G.Source.GetReference()==Source) && G.Resources==Generation;
        };
        if (S.Published && !HasAuthority(*S.Published))
        {
            RetirePublished_RenderThread(Commands, std::move(S.Published));
            S.Schedule = {};
        }
        if (S.Job)
        {
            const bool Current = HasAuthority(*S.Job->Output);
            const auto Timing = S.Job->Query && S.Job->Completion.IsComplete()
                ? GDynamicRHI->RHIGetGPUTimingResult(S.Job->Query) : FRHIGPUTimingResult{};
            auto Previous = S.Published;
            if (auto Finished = S.PollJob(Current, Light ? Light->UpdateStatus : nullptr,
                Timing.State == ERHIGPUTimingResultState::Ready ? double(Timing.DurationNanoseconds)/1.e6 : 0.0))
            {
                if (Current && !Finished->Completion.IsComplete() && Light)
                    S.Schedule.Fail({Finished->Output->Owner, Finished->Output->SourceEpoch,
                        Finished->Output->Provider, Finished->Output->Request},
                        Now(), Light->RefreshInterval);
                if (S.Published != Previous) RetirePublished_RenderThread(Commands, std::move(Previous));
                if (Finished->Completion.IsComplete() && Finished->Output->Resources == Generation)
                    State->Lut = Finished->Output->BrdfLut;
                Retirement.Retire(Finished->Output, Finished->Completion, Finished->Output->RHIDeviceGeneration);
            }
            if (S.Job) { S.bUpdatePending = true; return; }
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
                Add(G->Radiance,LightingBytes); Add(G->Irradiance,LightingBytes); Add(G->Prefiltered,LightingBytes); Add(G->BrdfLut,LightingBytes);
                Add(G->Source,SourceBytes);
            }
            if (Light) Light->UpdateStatus->RetainedImageBytes.store(LightingBytes);
            return std::pair(LightingBytes,SourceBytes);
        };
        // Request shaders/PSOs even while the source texture or sky provider is
        // loading. The ordinary scene selection still requires valid inputs.
        const auto PreparationLight = Light ? Light : PendingLight;
        if (!PreparationLight) return;
        const double Time=Now();
        const bool Manual=!Light || !S.Published || S.Published->Request!=Light->RequestSerial;
        const bool Changed=Captured && S.Published && S.Published->Revision!=Sky->Revision;
        if (Light && !Manual && !(Light->bAutomaticRefresh && Changed))
        {
            // A lower-priority ready light may be used while the preferred
            // source loads. Keep observing its stable texture reference.
            S.bUpdatePending = PendingLight != Light;
            Light->UpdateStatus->SteadyCPUMilliseconds.store((Now()-CPUStart)*1000);
            return;
        }
        S.bUpdatePending = true;
        const FSkyLightingRequestKey Request{PreparationLight->InstanceId, PreparationLight->SourceEpoch,
            PreparationLight->SourceMode == ESkyLightSourceMode::CapturedSky && Sky ? Sky->InstanceId : 0,
            PreparationLight->RequestSerial};
        if (!S.Schedule.CanAttempt(Request, Time, Manual, PreparationLight->RefreshInterval))
        {
            PreparationLight->UpdateStatus->SteadyCPUMilliseconds.store((Now()-CPUStart)*1000);
            return;
        }
        const auto* Caps=GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
        if (!Caps || !Caps->bSupportsSkyLighting)
        {
            PreparationLight->UpdateStatus->State = ESkyLightUpdateState::Unsupported;
            S.bUpdatePending = false;
            return;
        }
        const auto Availability = PrepareResources_RenderThread();
        if (Availability != ERenderResourceAvailability::Ready || !FallbackCube)
        {
            const bool Failed = Availability == ERenderResourceAvailability::Failed;
            PreparationLight->UpdateStatus->State = Failed ? ESkyLightUpdateState::Failed : ESkyLightUpdateState::Pending;
            if (Failed) S.Schedule.Fail(Request, Time, PreparationLight->RefreshInterval);
            return;
        }
        if (!Light || (Captured && !Sky)) return;
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
            return;
        }
        Light->UpdateStatus->State.store(ESkyLightUpdateState::Pending);
        auto Candidate=std::make_shared<FSkyLightingGeneration>();
        Candidate->Resources=Generation; Candidate->Owner=Light->InstanceId;
        Candidate->RHIDeviceGeneration=GDynamicRHI->RHIGetQueueCapabilities().DeviceGeneration;
        Candidate->Sampler=State->Sampler;
        Candidate->SourceEpoch=Light->SourceEpoch; Candidate->Provider=Provider;
        Candidate->Revision=Captured ? Sky->Revision : 0; Candidate->Request=Light->RequestSerial;
        Candidate->Source=Captured ? nullptr : Source; Candidate->CaptureTime=Time;
        const bool bTiming = GDynamicRHI->RHIGetCapabilities()->bSupportsGPUTimestamps;
        auto Query = bTiming ? GDynamicRHI->RHICreateGPUTimingQuery() : FGPUTimingQueryRHIRef{};
        // Timing pool exhaustion must not prevent a lighting update.
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
        if (!Result.has_value())
        {
            Light->UpdateStatus->State.store(ESkyLightUpdateState::Failed);
            S.Schedule.Fail(Request, Time, Light->RefreshInterval);
            return;
        }
        auto Completion = Commands.BeginGPUSubmission({.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
        Commands.EndGPUSubmission();
        Candidate->BrdfLut = Lut ? std::move(Lut) : State->Lut;
        S.Job = FSkyLightingJob{Candidate, std::move(Completion), std::move(Query)};
        S.Schedule.Submit(Time);
        S.Generations.push_back(Candidate);
        MeasureImages();
        Commands.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::SubmitToGPU);
    }
    auto FEnvironmentLightingResources::GetSceneBinding_RenderThread(const FScene* Scene) const
        -> std::shared_ptr<const FSkyLightingGeneration>
    {
        CheckRenderingThread();
        const auto Result = Scene ? Scene->SkyLighting->Published : nullptr;
        return Result && Result->Resources == Coordinator.GetGeneration_RenderThread() ? Result : nullptr;
    }
    auto FEnvironmentLightingResources::ReleaseScene_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
    {
        CheckRenderingThread();
        auto& S = *Scene.SkyLighting;
        RetirePublished_RenderThread(Commands, std::move(S.Published));
        if (S.Job)
        {
            Retirement.Retire(S.Job->Output, S.Job->Completion, S.Job->Output->RHIDeviceGeneration);
            S.Job.reset();
        }
        S.Schedule = {};
        S.bUpdatePending = false;
        // Keep the weak inventory across logical invalidation: physical leases still count.
    }
    auto FEnvironmentLightingResources::ReleaseResources_RenderThread(FRHICommandListImmediate& Commands) -> void
    {
        CheckRenderingThread();
        for (auto& Entry : Scenes)
            if (auto State = Entry.lock())
            {
                RetirePublished_RenderThread(Commands, std::move(State->Published));
                if (State->Job)
                {
                    Retirement.Retire(State->Job->Output, State->Job->Completion, State->Job->Output->RHIDeviceGeneration);
                    State->Job.reset();
                }
                State->Schedule = {};
                State->bUpdatePending = false;
            }
        State=std::make_unique<FState>();
    }
}
