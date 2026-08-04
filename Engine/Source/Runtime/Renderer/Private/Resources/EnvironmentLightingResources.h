#pragma once

#include "EnvironmentLighting/EnvironmentLighting.h"
#include "RenderResourceCreation.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRendererResourceCoordinator;

	// Renderer-owned GPU resources sourced from the built-in studio IBL asset.
	class FEnvironmentLightingResources
	{
	public:
		explicit FEnvironmentLightingResources(
			FRendererResourceCoordinator& InCoordinator);

		auto Initialize(
			std::shared_ptr<const FEnvironmentLightingData> InSourceData) -> void;
		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto GetIrradiance_RenderThread() const -> FRHITexture*;
		auto GetPrefiltered_RenderThread() const -> FRHITexture*;
		auto GetBrdfLut_RenderThread() const -> FRHITexture*;
		auto GetSampler_RenderThread() const -> FRHISampler*;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FPayload
		{
			FTextureRHIRef Irradiance;
			FTextureRHIRef Prefiltered;
			FTextureRHIRef BrdfLut;
			FSamplerRHIRef Sampler;
		};

		FRendererResourceCoordinator& Coordinator;
		std::shared_ptr<const FEnvironmentLightingData> SourceData;
		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Device
				| ERenderResourceGenerationDependency::Manual};
	};
} // namespace Durin
