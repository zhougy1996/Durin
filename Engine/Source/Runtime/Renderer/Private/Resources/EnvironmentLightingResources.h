#pragma once

#include "CoreMinimal.h"

#include "RenderResourceCreation.h"
#include "RHIResources.h"
#include "RHICompletion.h"
#include "Rendering/SkyLightSceneProxy.h"

namespace Durin
{
    class FRHICommandListImmediate;
    class FRendererResourceCoordinator;
    class FRendererRDGAllocator;
    class FScene;

    struct FSkyLightingGeneration
    {
        FTextureRHIRef Radiance, Irradiance, Prefiltered, BrdfLut;
        FRenderResourceGeneration Resources;
        uint64 Owner = 0, SourceEpoch = 0, Provider = 0, Revision = 0, Request = 0;
        FTextureRHIRef Source;
        double CaptureTime = 0;
    };

    // One GPU job per scene; requests coalesce to the latest scene snapshots.
    struct FSkyLightingSceneState
    {
        std::shared_ptr<const FSkyLightingGeneration> Active;
        std::shared_ptr<const FSkyLightingGeneration> InFlight;
        // Last-known-good output remains owned until replacement completes.
        std::shared_ptr<const FSkyLightingGeneration> Previous;
        struct FFailedGeneration
        {
            FRHIGPUSyncPointRef Completion;
            std::shared_ptr<const FSkyLightingGeneration> Generation;
        };
        // Failed native work cannot be retired by a later successful marker.
        std::vector<FFailedGeneration> FailedGenerations;
        bool bRetry = false;
        TRefCountPtr<FRHIGPUTimingQuery> Query;
        FRHIGPUSyncPointRef Completion;
        double LastAttempt = -60;
        uint64 AttemptOwner=0, AttemptEpoch=0, AttemptProvider=0, AttemptRequest=0;
        uint64 WorldUpdateFrame=std::numeric_limits<uint64>::max();
        std::vector<std::weak_ptr<const FSkyLightingGeneration>> Generations;
        std::vector<std::shared_ptr<const FSkyLightingGeneration>> Retiring;

        // Called only for a terminal job/retirement marker on the rendering thread.
        auto FinishCompletion(bool bCurrentAuthority, bool bPreviousAuthority,
            const std::shared_ptr<FSkyLightUpdateStatus>& Status, double Milliseconds) -> void
        {
            const bool bComplete = Completion.IsComplete();
            if (bComplete)
            {
                if (InFlight && bCurrentAuthority)
                {
                    Status->UpdateMilliseconds.store(Milliseconds);
                    Status->CompletedUpdates.fetch_add(1, std::memory_order_release);
                }
                Previous.reset();
                if (!InFlight || bCurrentAuthority) Retiring.clear();
                // A later ordered marker covers consumers recorded after cancellation.
                std::erase_if(FailedGenerations, [](const auto& Entry) {
                    return Entry.Completion.IsRetirementEligible();
                });
            }
            else
            {
                if (InFlight)
                {
                    if (Active == InFlight)
                        Active = bPreviousAuthority ? std::move(Previous) : nullptr;
                    FailedGenerations.push_back({Completion, InFlight});
                    bRetry = true;
                    if (bCurrentAuthority)
                    {
                        Status->PublishedRevision.store(Active ? Active->Revision : 0);
                        Status->CaptureTime.store(Active ? Active->CaptureTime : 0);
                        Status->State.store(ESkyLightUpdateState::Failed, std::memory_order_release);
                    }
                }
                if (Previous) Retiring.push_back(std::move(Previous));
            }
            InFlight.reset();
            Query = nullptr;
            Completion = nullptr;
        }

    };

    // Shares the compute pipeline and LUT. Scenes own all environment generations.
    class FEnvironmentLightingResources
    {
    public:
        explicit FEnvironmentLightingResources(FRendererResourceCoordinator& InCoordinator);
        ~FEnvironmentLightingResources();
        auto UpdateScene_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene,
            FRendererRDGAllocator& Allocator, FRHITexture* FallbackCube) -> void;
        auto SelectScene_RenderThread(const FScene* Scene) -> void;
        auto EnsureResources_RenderThread(FRHICommandListImmediate&) -> bool;
        auto GetIrradiance_RenderThread() const -> FRHITexture*;
        auto GetPrefiltered_RenderThread() const -> FRHITexture*;
        auto GetBrdfLut_RenderThread() const -> FRHITexture*;
        auto GetSampler_RenderThread() const -> FRHISampler*;
        auto ReleaseResources_RenderThread() -> void;
    private:
        struct FState;
        std::unique_ptr<FState> State;
        FRendererResourceCoordinator& Coordinator;
        std::weak_ptr<const FSkyLightingGeneration> Selected;
        std::vector<std::weak_ptr<FSkyLightingSceneState>> Scenes;
    };
}
