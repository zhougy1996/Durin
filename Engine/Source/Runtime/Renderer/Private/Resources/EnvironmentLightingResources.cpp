#include "Resources/EnvironmentLightingResources.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto UploadCube(
			FRHICommandListImmediate& CommandList,
			const char* Name,
			uint32 Dimension,
			uint32 MipCount,
			const FEnvironmentLightingData& Data,
			bool bIrradiance) -> FTextureRHIRef
		{
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube(Name)
				.SetExtent(Dimension)
				.SetNumMips(MipCount)
				.SetFormat(EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture == nullptr) return nullptr;
			for (uint32 Mip = 0; Mip < MipCount; ++Mip)
			{
				const uint32 MipDimension = std::max(Dimension >> Mip, 1u);
				for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
				{
					const std::vector<uint16>& Pixels = bIrradiance
						? Data.Irradiance[Face]
						: Data.Prefiltered[Mip][Face];
					GDynamicRHI->RHIUpdateTexture2D(
						CommandList, Texture, Mip, Face,
						FUpdateTextureRegion2D(
							0, 0, 0, 0, MipDimension, MipDimension),
						MipDimension * 8,
						std::as_bytes(std::span(Pixels)));
				}
			}
			return Texture;
		}
	}

	FEnvironmentLightingResources::FEnvironmentLightingResources(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
	{
	}

	auto FEnvironmentLightingResources::Initialize(
		std::shared_ptr<const FEnvironmentLightingData> InSourceData) -> void
	{
		check(IsInGameThread());
		SourceData = std::move(InSourceData);
	}

	auto FEnvironmentLightingResources::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		check(IsInRenderingThread());
		const std::shared_ptr<const FEnvironmentLightingData> Data = SourceData;
		if (!Data || !Data->IsValid()) return false;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[&CommandList, Data]() -> FResult {
				FPayload Candidate;
				Candidate.Irradiance = UploadCube(
					CommandList, "StudioEnvironmentIrradiance",
					EnvironmentIrradianceDimension, 1, *Data, true);
				Candidate.Prefiltered = UploadCube(
					CommandList, "StudioEnvironmentPrefiltered",
					EnvironmentPrefilterDimension,
					EnvironmentPrefilterMipCount, *Data, false);
				const FRHITextureCreateDesc LutDesc = FRHITextureCreateDesc::Create2D(
					"StudioEnvironmentBrdfLut",
					EnvironmentBrdfLutDimension,
					EnvironmentBrdfLutDimension,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::ShaderResource);
				Candidate.BrdfLut = GDynamicRHI->RHICreateTexture(CommandList, LutDesc);
				if (Candidate.BrdfLut != nullptr)
				{
					GDynamicRHI->RHIUpdateTexture2D(
						CommandList, Candidate.BrdfLut, 0, 0,
						FUpdateTextureRegion2D(
							0, 0, 0, 0,
							EnvironmentBrdfLutDimension,
							EnvironmentBrdfLutDimension),
						EnvironmentBrdfLutDimension * 8,
						std::as_bytes(std::span(Data->BrdfLut)));
				}
				Candidate.Sampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
				if (Candidate.Irradiance == nullptr || Candidate.Prefiltered == nullptr
					|| Candidate.BrdfLut == nullptr || Candidate.Sampler == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"EnvironmentLightingResources",
						"built-in-studio-v1",
						"Environment texture or sampler creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic) != nullptr;
	}

	auto FEnvironmentLightingResources::GetIrradiance_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->Irradiance.GetReference() : nullptr;
	}

	auto FEnvironmentLightingResources::GetPrefiltered_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->Prefiltered.GetReference() : nullptr;
	}

	auto FEnvironmentLightingResources::GetBrdfLut_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->BrdfLut.GetReference() : nullptr;
	}

	auto FEnvironmentLightingResources::GetSampler_RenderThread() const -> FRHISampler*
	{
		check(IsInRenderingThread());
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->Sampler.GetReference() : nullptr;
	}

	auto FEnvironmentLightingResources::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		Slot.Reset();
	}
}
