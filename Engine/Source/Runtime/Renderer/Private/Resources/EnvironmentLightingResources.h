#pragma once

#include "CoreMinimal.h"

#include "RendererAPI.h"
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
    class FRendererResourceRetirementQueue;

    struct FSkyLightingGeneration
    {
        FTextureRHIRef Radiance, Irradiance, Prefiltered, BrdfLut;
        FRenderResourceGeneration Resources;
        uint64 RHIDeviceGeneration = 0;
        FSamplerRHIRef Sampler;
        uint64 Owner = 0, SourceEpoch = 0, Provider = 0, Revision = 0, Request = 0;
        FTextureRHIRef Source;
        double CaptureTime = 0;
    };

    struct FSkyLightingJob
    {
        std::shared_ptr<const FSkyLightingGeneration> Output;
        FRHIGPUSyncPointRef Completion;
        FGPUTimingQueryRHIRef Query;

    };

    struct FSkyLightingRequestKey
    {
        uint64 Owner = 0, SourceEpoch = 0, Provider = 0, Request = 0;
        auto operator==(const FSkyLightingRequestKey&) const -> bool = default;
    };

    // Published is always a completed result. Jobs own unpublished candidates.
    struct FSkyLightingSceneState
    {
        std::shared_ptr<const FSkyLightingGeneration> Published;
        std::optional<FSkyLightingJob> Job;
        double LastAttempt = -60;
        std::optional<FSkyLightingRequestKey> LastRequest;
        uint64 WorldUpdateFrame=std::numeric_limits<uint64>::max();
        std::vector<std::weak_ptr<const FSkyLightingGeneration>> Generations;

        // Removes terminal work from scheduling; the caller retires its resource lease.
        RENDERER_API auto PollJob(bool bCurrentAuthority,
            const std::shared_ptr<FSkyLightUpdateStatus>& Status, double Milliseconds)
            -> std::optional<FSkyLightingJob>;
    };

    // Shares the compute pipeline and LUT. Scenes own all environment generations.
    class FEnvironmentLightingResources
    {
    public:
        explicit FEnvironmentLightingResources(FRendererResourceCoordinator& InCoordinator, FRendererResourceRetirementQueue& InRetirement);
        ~FEnvironmentLightingResources();
        auto UpdateScene_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene,
            FRendererRDGAllocator& Allocator, FRHITexture* FallbackCube) -> void;
        auto GetSceneBinding_RenderThread(const FScene* Scene) const -> std::shared_ptr<const FSkyLightingGeneration>;
        auto ReleaseScene_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void;
        auto ReleaseResources_RenderThread(FRHICommandListImmediate& Commands) -> void;
    private:
        auto PrepareResources_RenderThread() -> ERenderResourceAvailability;
        auto RetirePublished_RenderThread(FRHICommandListImmediate& Commands,
            std::shared_ptr<const FSkyLightingGeneration> Generation) -> void;
        FRendererResourceRetirementQueue& Retirement;
        struct FState;
        std::unique_ptr<FState> State;
        FRendererResourceCoordinator& Coordinator;
        std::vector<std::weak_ptr<FSkyLightingSceneState>> Scenes;
    };
}
