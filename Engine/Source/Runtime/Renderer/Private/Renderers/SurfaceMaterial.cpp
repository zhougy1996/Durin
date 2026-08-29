#include "Renderers/SurfaceMaterial.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "RenderingThread.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"

#include <bit>
#include <format>

namespace Durin::RendererPrivate
{
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceFragmentShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader);
	DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FSurfaceOpaqueShadowFragmentShader);

	namespace
	{
		auto CreateSurfaceSampler(const FMaterialSamplerState& State)
			-> TRenderResourceCreateResult<FSamplerRHIRef>
		{
			using FResult = TRenderResourceCreateResult<FSamplerRHIRef>;
			FSamplerRHIRef Candidate = RHICreateSampler(MakeMaterialSamplerDesc(State));
			if (Candidate != nullptr) return FResult::Success(std::move(Candidate));
			return FResult::Failure(MakeRendererResourceCreateError(
				ERenderResourceCreateErrorCategory::RHIResource,
				"SurfaceMaterialSampler",
				std::format("min={},mag={},u={},v={}",
					static_cast<uint8>(State.MinFilter),
					static_cast<uint8>(State.MagFilter),
					static_cast<uint8>(State.AddressU),
					static_cast<uint8>(State.AddressV)),
				"RHI sampler creation returned null.",
				ERenderResourceGenerationDependency::Device
					| ERenderResourceGenerationDependency::Manual));
		}
	}

	struct FSurfaceMaterialResources::FState
	{
		TRendererResourceSlotCache<FMaterialSamplerState, FSamplerRHIRef> Samplers{
			ERenderResourceGenerationDependency::Device};
		FSurfaceMaterialResourceCounters Counters;
	};

	FSurfaceMaterialResources::FSurfaceMaterialResources(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures,
		FEnvironmentLightingResources& InEnvironmentLighting)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, EnvironmentLighting(InEnvironmentLighting)
		, State(std::make_unique<FState>())
	{
	}

	FSurfaceMaterialResources::~FSurfaceMaterialResources() = default;

	auto FSurfaceMaterialResources::Ensure_RenderThread(
		const FMaterialRenderBinding& Binding,
		ESurfaceMaterialPass Pass) -> bool
	{
		check(IsInRenderingThread());
		const uint8 RequiredRoles = GetSurfaceMaterialRequiredRoleMask(Pass);
		for (size_t Role = 0; Role < SurfaceMaterialRoleCount; ++Role)
		{
			if ((RequiredRoles & (uint8{1} << Role)) == 0) continue;
			++State->Counters.SamplerLookups;
			const FMaterialSamplerState StateKey = Binding.Samplers[Role];
			auto& Entry = State->Samplers.FindOrAdd(StateKey);
			bool bCreationAttempted = false;
			if (Entry.Slot.Resolve(
					Coordinator.GetGeneration_RenderThread(),
					[StateKey, &bCreationAttempted] {
						bCreationAttempted = true;
						return CreateSurfaceSampler(StateKey);
					},
					ReportRendererResourceCreateDiagnostic) == nullptr)
			{
				++State->Counters.SamplerFailures;
				check(State->Counters.IsConserved());
				return false;
			}
			if (bCreationAttempted)
				++State->Counters.SamplerCreations;
			else
				++State->Counters.SamplerReuses;
			check(State->Counters.IsConserved());
		}
		return true;
	}

	auto FSurfaceMaterialResources::Resolve_RenderThread(
		const FMaterialRenderBinding& Binding,
		ESurfaceMaterialPass Pass,
		bool bLit,
		bool bEnableSpecularAA,
		FRHITexture* DirectionalShadowTexture,
		FRHISampler* DirectionalShadowSampler,
		FResolvedSurfaceMaterial& OutMaterial) const -> bool
	{
		check(IsInRenderingThread());
		OutMaterial = {};
		if (Pass == ESurfaceMaterialPass::OpaqueShadow) return true;

		OutMaterial.Uniform = MakeSurfaceMaterialUniform(
			Binding, bLit, bEnableSpecularAA);
		const uint8 RequiredRoles = GetSurfaceMaterialRequiredRoleMask(Pass);
		const FRenderResourceGeneration Generation =
			Coordinator.GetGeneration_RenderThread();
		for (size_t Role = 0; Role < SurfaceMaterialRoleCount; ++Role)
		{
			if ((RequiredRoles & (uint8{1} << Role)) == 0) continue;
			const auto* Entry = State->Samplers.Find(Binding.Samplers[Role]);
			if (Entry == nullptr
				|| HasSelectedRenderResourceGenerationChanged(
					Entry->Slot.GetPayloadGeneration(), Generation,
					ERenderResourceGenerationDependency::Device))
			{
				return false;
			}
			const FSamplerRHIRef* Sampler = Entry->Slot.GetPayload();
			if (Sampler == nullptr) return false;
			FRHITexture* Texture = Binding.Textures[Role] != nullptr
				? Binding.Textures[Role]->GetReferencedTexture_RenderThread()
				: nullptr;
			OutMaterial.Textures[Role] = Texture != nullptr
				? Texture : DefaultTextures.Get_RenderThread(SurfaceTextureFallbacks[Role]);
			OutMaterial.Samplers[Role] = Sampler->GetReference();
			if (OutMaterial.Textures[Role] == nullptr
				|| OutMaterial.Samplers[Role] == nullptr) return false;
			OutMaterial.ResolvedRoleMask |= uint8{1} << Role;
		}

		if (Pass != ESurfaceMaterialPass::Forward) return true;
		FRHITexture* Irradiance = EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* Prefiltered = EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* Brdf = EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler = EnvironmentLighting.GetSampler_RenderThread();
		const bool bCompleteEnvironment = Irradiance != nullptr
			&& Prefiltered != nullptr && Brdf != nullptr
			&& EnvironmentSampler != nullptr;
		OutMaterial.EnvironmentIrradiance = bCompleteEnvironment
			? Irradiance : DefaultTextures.GetCube_RenderThread();
		OutMaterial.EnvironmentPrefiltered = bCompleteEnvironment
			? Prefiltered : DefaultTextures.GetCube_RenderThread();
		OutMaterial.EnvironmentBrdfLut = bCompleteEnvironment
			? Brdf : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
		OutMaterial.EnvironmentSampler = bCompleteEnvironment
			? EnvironmentSampler : OutMaterial.Samplers[0];
		OutMaterial.DirectionalShadowTexture = DirectionalShadowTexture != nullptr
			? DirectionalShadowTexture : DefaultTextures.GetArray_RenderThread();
		OutMaterial.DirectionalShadowSampler = DirectionalShadowSampler != nullptr
			? DirectionalShadowSampler : OutMaterial.Samplers[0];
		return OutMaterial.EnvironmentIrradiance != nullptr
			&& OutMaterial.EnvironmentPrefiltered != nullptr
			&& OutMaterial.EnvironmentBrdfLut != nullptr
			&& OutMaterial.EnvironmentSampler != nullptr
			&& OutMaterial.DirectionalShadowTexture != nullptr
			&& OutMaterial.DirectionalShadowSampler != nullptr;
	}

	auto FSurfaceMaterialResources::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Samplers.Reset();
		State->Counters = {};
	}

	auto FSurfaceMaterialResources::GetSamplerSlotCount_RenderThread() const -> size_t
	{
		check(IsInRenderingThread());
		return State->Samplers.Num();
	}

	auto FSurfaceMaterialResources::GetResourceCounters_RenderThread() const
		-> FSurfaceMaterialResourceCounters
	{
		check(IsInRenderingThread());
		check(State->Counters.IsConserved());
		return State->Counters;
	}

	auto MakeSurfaceForwardParameters(
		const FResolvedSurfaceMaterial& Material,
		const FRHIUniformBufferRange& MaterialBuffer,
		const FRHIUniformBufferRange& Lighting)
		-> FSurfaceFragmentShader::FParameters
	{
		FSurfaceFragmentShader::FParameters Result;
		Result.Lighting = Lighting;
		Result.Material = MaterialBuffer;
		Result.BaseColorTexture = Material.Textures[0];
		Result.NormalTexture = Material.Textures[1];
		Result.MetallicTexture = Material.Textures[2];
		Result.RoughnessTexture = Material.Textures[3];
		Result.AmbientOcclusionTexture = Material.Textures[4];
		Result.EmissiveTexture = Material.Textures[5];
		Result.OpacityTexture = Material.Textures[6];
		Result.OpacityMaskTexture = Material.Textures[7];
		Result.BaseColorSampler = Material.Samplers[0];
		Result.NormalSampler = Material.Samplers[1];
		Result.MetallicSampler = Material.Samplers[2];
		Result.RoughnessSampler = Material.Samplers[3];
		Result.AmbientOcclusionSampler = Material.Samplers[4];
		Result.EmissiveSampler = Material.Samplers[5];
		Result.OpacitySampler = Material.Samplers[6];
		Result.OpacityMaskSampler = Material.Samplers[7];
		Result.EnvironmentIrradiance = Material.EnvironmentIrradiance;
		Result.EnvironmentPrefiltered = Material.EnvironmentPrefiltered;
		Result.EnvironmentBrdfLut = Material.EnvironmentBrdfLut;
		Result.EnvironmentSampler = Material.EnvironmentSampler;
		Result.DirectionalShadowTexture = Material.DirectionalShadowTexture;
		Result.DirectionalShadowSampler = Material.DirectionalShadowSampler;
		return Result;
	}

	auto MakeSurfaceMaskedShadowParameters(
		const FResolvedSurfaceMaterial& Material,
		const FRHIUniformBufferRange& MaterialBuffer)
		-> FSurfaceMaskedShadowFragmentShader::FParameters
	{
		FSurfaceMaskedShadowFragmentShader::FParameters Result;
		Result.Material = MaterialBuffer;
		Result.OpacityMaskTexture = Material.Textures[7];
		Result.OpacityMaskSampler = Material.Samplers[7];
		return Result;
	}
} // namespace Durin::RendererPrivate
