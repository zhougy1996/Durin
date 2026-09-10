#pragma once

#include "RenderResourceCreation.h"
#include "RHIResources.h"
#include "Rendering/SkyLightSceneProxy.h"

namespace Durin
{
    class FRHICommandListImmediate;
    class FRendererResourceCoordinator;
    class FRendererRDGAllocator;
    class FScene;

    struct FSkyLightingGeneration
    {
        FTextureRHIRef Radiance, Irradiance, Prefiltered;
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
        TRefCountPtr<FRHIGPUTimingQuery> Query;
        double LastAttempt = -60;
        uint64 AttemptOwner=0, AttemptEpoch=0, AttemptProvider=0, AttemptRequest=0;
        uint64 WorldUpdateFrame=std::numeric_limits<uint64>::max();
        std::vector<std::weak_ptr<const FSkyLightingGeneration>> Generations;
        std::vector<std::shared_ptr<const FSkyLightingGeneration>> Retiring;
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
