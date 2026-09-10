#include "SkyLightingRuntimeSmoke.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"

#include "Actors/ProceduralSkyActor.h"
#include "Actors/SkyLightActor.h"
#include "Components/ProceduralSkyComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "EngineGlobals.h"
#include "Rendering/SkyLightSceneProxy.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "IRendererModule.h"

namespace Durin
{
    namespace
    {
        const auto ProcessStart=std::chrono::steady_clock::now();
        struct FFrameSample
        {
            FGPUTimingQueryRHIRef Frame, Service;
            bool Updated=false, Baseline=false;
        };
        // Exclusively render-thread owned; allocated only by the opt-in smoke.
        struct FFrameMeasurements
        {
            std::shared_ptr<const FSkyLightUpdateStatus> Status;
            std::vector<FFrameSample> Pending;
            std::vector<double> SteadyGPU, UpdateCPU;
            uint64 BeforeRevision=0, BaselineTicks=0;
            uint32 OverdueTicks=0, MaxOverdueTicks=0;
            bool Baseline=false, Recording=false;
            double FramePeak=0, LastCapture=0, MaxInterval=0;
            std::chrono::steady_clock::time_point CPUStart;
        };
        std::unique_ptr<FFrameMeasurements> Measurements;
        IRendererModule* TimingRenderer=nullptr;
    }

    auto BeginSkyLightingSmokeFrame(FRHICommandListImmediate& Commands) -> void
    {
        if (!Measurements) return;
        auto& M=*Measurements;
        // Query each command-list-local segment: swapchain acquire/present
        // may synchronously submit and cannot have an open timestamp scope.
        if (M.Status->CompletedUpdates.load()<30) return;
        M.Recording=true;
        TimingRenderer->SetViewGPUTimingSink_RenderThread([](FGPUTimingQueryRHIRef Query) {
            if (Measurements && Measurements->Recording) Measurements->Pending.back().Frame=std::move(Query);
        });
        std::erase_if(M.Pending,[&](const FFrameSample& S) {
            if (!S.Frame) return true;
            const auto Frame=GDynamicRHI->RHIGetGPUTimingResult(S.Frame);
            const auto Service=GDynamicRHI->RHIGetGPUTimingResult(S.Service);
            if (Frame.State==ERHIGPUTimingResultState::Pending || Service.State==ERHIGPUTimingResultState::Pending) return false;
            check(Frame.State==ERHIGPUTimingResultState::Ready && Service.State==ERHIGPUTimingResultState::Ready);
            if (S.Updated) M.FramePeak=std::max(M.FramePeak,double(Frame.DurationNanoseconds+Service.DurationNanoseconds)/1.e6);
            if (S.Baseline) M.SteadyGPU.push_back(double(Service.DurationNanoseconds)/1.e6);
            return true;
        });
        FFrameSample Sample;
        Sample.Service=GDynamicRHI->RHICreateGPUTimingQuery();
        check(Sample.Service);
        Sample.Baseline=M.Baseline && ++M.BaselineTicks>30;
        M.BeforeRevision=M.Status->PublishedRevision.load();
        Commands.BeginGPUTimingQuery(Sample.Service);
        M.Pending.push_back(std::move(Sample));
        M.CPUStart=std::chrono::steady_clock::now();
    }

    auto AfterSkyLightingSmokeUpdate(FRHICommandListImmediate& Commands) -> void
    {
        if (!Measurements || !Measurements->Recording) return;
        auto& M=*Measurements;
        auto& Sample=M.Pending.back();
        Commands.EndGPUTimingQuery(Sample.Service);
        Sample.Updated=M.BeforeRevision!=M.Status->PublishedRevision.load() && M.Status->CompletedUpdates.load()>=30;
        if (Sample.Updated)
        {
            M.UpdateCPU.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-M.CPUStart).count());
            const double Capture=M.Status->CaptureTime.load();
            if (!M.Baseline && M.LastCapture>0 && M.Status->CompletedUpdates.load()>30)
            {
                M.MaxInterval=std::max(M.MaxInterval,Capture-M.LastCapture);
                M.MaxOverdueTicks=std::max(M.MaxOverdueTicks,M.OverdueTicks);
            }
            M.OverdueTicks=0;
            M.LastCapture=Capture;
        }
        else if (!M.Baseline && M.LastCapture>0)
        {
            const double Now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (Now-M.LastCapture>=0.25) ++M.OverdueTicks;
        }
    }

    auto EndSkyLightingSmokeFrame(FRHICommandListImmediate& Commands) -> void
    {
        if (Measurements && Measurements->Recording)
        {
            Measurements->Recording=false;
        }
    }

    struct FSkyLightingRuntimeSmokeState
    {
        AProceduralSkyActor* Sky = nullptr;
        ASkyLightActor* Light = nullptr;
        uint64 Ticks = 0, Completed = 0, PeakBytes = 0;
        uint32 Batch = 0;
        uint32 BaselineTicks = 0;
        double ManualCaptureTime = 0;
        std::vector<double> SteadyCPU;
        std::vector<double> Samples;
        std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
    };

    auto BeginSkyLightingRuntimeSmoke() -> std::shared_ptr<FSkyLightingRuntimeSmokeState>
    { return std::make_shared<FSkyLightingRuntimeSmokeState>(); }

    auto TickSkyLightingRuntimeSmoke(const std::shared_ptr<FSkyLightingRuntimeSmokeState>& State) -> bool
    {
        auto& S = *State;
        const double Age = std::chrono::duration<double>(std::chrono::steady_clock::now()-S.Start).count();
        checkf(Age < 300, "Sky lighting smoke timed out waiting for GPU updates.");
        auto* World = GEngine ? GEngine->GetWorld() : nullptr;
        if (!World || !World->GetCurrentLevel()) return false;
        if (!S.Light)
        {
            // First prove the authored ordinary cube survived package loading.
            bool StudioReady = false;
            for (const auto& Actor : World->GetCurrentLevel()->GetActors())
                if (auto* Light = Cast<ASkyLightActor>(Actor.Get()))
                    StudioReady |= Light->GetSkyLightComponent()->GetSourceMode()==ESkyLightSourceMode::SpecifiedCube
                        && Light->GetSkyLightComponent()->GetUpdateStatus()->CompletedUpdates.load()>0;
            if (!StudioReady) return false;
            for (const auto& Actor : World->GetCurrentLevel()->GetActors())
                if (auto* Light = Cast<ASkyLightActor>(Actor.Get()))
                {
                    const double Initial=Light->GetSkyLightComponent()->GetUpdateStatus()->UpdateMilliseconds.load();
                    DURIN_INFO("Sky lighting initial update including LUT: {} ms.",Initial);
                    checkf(Initial<=20,"Initial sky lighting and LUT exceeded budget.");
                }
            const auto& Windows=Mona::FMonaApplication::Get().GetWindows();
            if (!Windows.empty()) Windows.front()->ReshapeWindow({0,0},{1920,1080});
            S.Sky = World->SpawnActor<AProceduralSkyActor>("SkyLightingSmokeSky");
            S.Light = World->SpawnActor<ASkyLightActor>("SkyLightingSmokeLight");
            check(S.Sky && S.Light);
            S.Sky->GetSkyComponent()->SetPriority(1000);
            S.Light->GetSkyLightComponent()->SetPriority(1000);
            S.Light->GetSkyLightComponent()->SetSource(ESkyLightSourceMode::CapturedSky,nullptr);
            S.Light->GetSkyLightComponent()->SetRefreshPolicy(true,0.25f);
            const auto Status=S.Light->GetSkyLightComponent()->GetUpdateStatus();
            auto* Renderer=GEngine->GetRendererModule();
            TryEnqueueRenderCommand("BeginSkyFrameQualification",[Status,Renderer](FRHICommandListImmediate&) {
                TimingRenderer=Renderer;
                Measurements=std::make_unique<FFrameMeasurements>(); Measurements->Status=Status;
            });
            DURIN_INFO("Sky lighting startup wall time from Launch module initialization: {} s.",
                std::chrono::duration<double>(std::chrono::steady_clock::now()-ProcessStart).count());
            DURIN_INFO("Sky lighting smoke: authored HDR cube ready; starting continuous sky animation.");
        }
        if (S.Batch==3)
        {
            if (++S.BaselineTicks==1)
            {
                S.Light->GetSkyLightComponent()->SetRefreshPolicy(false,0.25f);
                TryEnqueueRenderCommand("BeginSkySteadyBaseline",[](FRHICommandListImmediate&) { Measurements->Baseline=true; });
            }
            if (S.BaselineTicks<180) return false;
            if (S.BaselineTicks==180)
            {
                S.ManualCaptureTime=S.Light->GetSkyLightComponent()->GetUpdateStatus()->CaptureTime.load();
                S.Light->GetSkyLightComponent()->Recapture();
                return false;
            }
            const bool Published=S.Light->GetSkyLightComponent()->GetUpdateStatus()->CaptureTime.load()!=S.ManualCaptureTime;
            checkf(S.BaselineTicks<=182,"Manual recapture did not publish within two scene ticks.");
            if (!Published) return false;
            DURIN_INFO("Sky lighting manual recapture published after {} scene tick(s).",S.BaselineTicks-180);
            TryEnqueueRenderCommand("ReportSkyFrameQualification",[](FRHICommandListImmediate&) {
                auto& M=*Measurements;
                check(M.SteadyGPU.size()>=120 && !M.UpdateCPU.empty());
                std::sort(M.SteadyGPU.begin(),M.SteadyGPU.end());
                std::sort(M.UpdateCPU.begin(),M.UpdateCPU.end());
                const double GPUP95=M.SteadyGPU[M.SteadyGPU.size()*95/100];
                DURIN_INFO("Sky lighting frame qualification: update scene GPU busy peak={} ms, disabled-update service GPU p95={} ms, admission/publication CPU p95={} ms, max automatic capture interval={} s.",
                    M.FramePeak,GPUP95,M.UpdateCPU[M.UpdateCPU.size()*95/100],M.MaxInterval);
                DURIN_INFO("Sky lighting automatic publication: maximum {} scene tick(s) past the refresh deadline.",M.MaxOverdueTicks);
                checkf(GPUP95<=0.1 && M.MaxOverdueTicks<=2,"Sky steady GPU/automatic latency exceeded budget.");
                TimingRenderer->SetViewGPUTimingSink_RenderThread({});
                Measurements.reset(); TimingRenderer=nullptr;
            });
            FlushRenderingCommands();
            return true;
        }
        const float Angle = float(++S.Ticks)*0.01f;
        S.Sky->GetSkyComponent()->SetSunDirection({std::cos(Angle),std::sin(Angle),0.75f});
        const auto Status = S.Light->GetSkyLightComponent()->GetUpdateStatus();
        const uint64 Completed = Status->CompletedUpdates.load(std::memory_order_acquire);
        if (Completed != S.Completed)
        {
            S.Completed = Completed;
            if (Completed>30) S.Samples.push_back(Status->UpdateMilliseconds.load());
            S.PeakBytes=std::max(S.PeakBytes,Status->RetainedImageBytes.load());
        }
        S.SteadyCPU.push_back(Status->SteadyCPUMilliseconds.load());
        if (S.Samples.size()<120) return false;
        std::sort(S.Samples.begin(),S.Samples.end());
        DURIN_INFO("Sky lighting smoke passed: updates={}, median={} ms, p95={} ms, max={} ms, elapsed={} s.",
            S.Samples.size(),S.Samples[60],S.Samples[113],S.Samples.back(),Age);
        checkf(S.Samples[60]<=4 && S.Samples[113]<=6 && S.Samples.back()<=8,
            "Dynamic sky update exceeded the supported GPU budget.");
        std::sort(S.SteadyCPU.begin(),S.SteadyCPU.end());
        const double CPUP95=S.SteadyCPU[S.SteadyCPU.size()*95/100];
        DURIN_INFO("Sky lighting smoke batch {}: retained image peak={} bytes, steady CPU p95={} ms.",++S.Batch,S.PeakBytes,CPUP95);
        checkf(S.PeakBytes>0 && S.PeakBytes<=16ull*1024*1024 && CPUP95<=0.2,"Sky lighting exceeded memory/CPU budget.");
        S.Samples.clear(); S.SteadyCPU.clear();
        return false;
    }

    auto EndSkyLightingRuntimeSmoke(std::shared_ptr<FSkyLightingRuntimeSmokeState>& State) -> void
    {
        if (!State) return;
        TryEnqueueRenderCommand("EndSkyFrameQualification",[](FRHICommandListImmediate&) {
            if (TimingRenderer) TimingRenderer->SetViewGPUTimingSink_RenderThread({});
            Measurements.reset(); TimingRenderer=nullptr;
        });
        FlushRenderingCommands();
        if (auto* World = GEngine ? GEngine->GetWorld() : nullptr)
        {
            if (State->Light) World->DestroyActor(State->Light);
            if (State->Sky) World->DestroyActor(State->Sky);
        }
        State.reset();
    }
}
