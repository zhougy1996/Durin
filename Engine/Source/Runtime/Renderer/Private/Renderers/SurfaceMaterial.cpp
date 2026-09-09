#include "Renderers/SurfaceMaterial.h"
#include "Shader/ShaderCookedLibrary.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "RenderingThread.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"

#include <bit>
#include <cstring>
#include <format>

namespace Durin::RendererPrivate
{
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceFragmentShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader);
	DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FSurfaceOpaqueShadowFragmentShader);
	const auto GOpaqueShadowRequest = [] {
		const FShaderType* Type = &FSurfaceOpaqueShadowFragmentShader::StaticType();
		return RegisterShaderRuntimeRequest({.Category = EShaderRuntimeRequestCategory::FeatureProgram,
			.Owner = "SurfaceMaterial", .Name = "Surface.OpaqueShadow"},
			EShaderRequestEligibility::GameAndEditor, std::span(&Type, 1));
	}();

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
		if (Pass == ESurfaceMaterialPass::OpaqueShadow || Binding.bError) return true;
		if (Binding.LayoutIdentity.Version != CompiledMaterialRenderLayoutVersion) return false;
		std::vector<FMaterialSamplerState> RequiredSamplers = Binding.CompiledSamplers;
		if (Pass == ESurfaceMaterialPass::Forward) RequiredSamplers.emplace_back();
		for (size_t Role = 0; Role < RequiredSamplers.size(); ++Role)
		{
			++State->Counters.SamplerLookups;
			const FMaterialSamplerState StateKey = RequiredSamplers[Role];
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

		const FRenderResourceGeneration Generation =
			Coordinator.GetGeneration_RenderThread();
		OutMaterial.bCompiledLayout = Binding.LayoutIdentity.Version == CompiledMaterialRenderLayoutVersion;
		if (OutMaterial.bCompiledLayout)
		{
			const size_t Count = Binding.CompiledTextures.size();
			if (Binding.CompiledSamplers.size() != Count || Binding.CompiledTextureFallbacks.size() != Count
				|| Binding.CompiledUniformPayload.size() < MaterialUniformControlBytes) return false;
			OutMaterial.CompiledUniformPayload = Binding.CompiledUniformPayload;
			const FVector4f Controls(0.0f, 0.0f, bLit ? 1.0f : 0.0f, bLit && bEnableSpecularAA ? 1.0f : 0.0f);
			std::memcpy(OutMaterial.CompiledUniformPayload.data(), &Controls, sizeof(Controls));
			if (Binding.bError) return Count == 0;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const auto* Entry = State->Samplers.Find(Binding.CompiledSamplers[Index]);
				if (!Entry || HasSelectedRenderResourceGenerationChanged(Entry->Slot.GetPayloadGeneration(),
					Generation, ERenderResourceGenerationDependency::Device) || !Entry->Slot.GetPayload()) return false;
				FRHITexture* Texture = Binding.CompiledTextures[Index] != nullptr
					? Binding.CompiledTextures[Index]->GetReferencedTexture_RenderThread() : nullptr;
				EDefaultTexture Fallback;
				switch (Binding.CompiledTextureFallbacks[Index])
				{
				case EMaterialTextureFallback::White: Fallback = EDefaultTexture::White; break;
				case EMaterialTextureFallback::Black: Fallback = EDefaultTexture::Black; break;
				case EMaterialTextureFallback::FlatRGNormal: Fallback = EDefaultTexture::FlatNormal; break;
				default: return false;
				}
				if (!Texture) Texture = DefaultTextures.Get_RenderThread(Fallback);
				FRHISampler* Sampler = Entry->Slot.GetPayload()->GetReference();
				if (!Texture || !Sampler) return false;
				OutMaterial.CompiledTextures.push_back(Texture);
				OutMaterial.CompiledSamplers.push_back(Sampler);
			}
		}
		else return false;

		if (Pass != ESurfaceMaterialPass::Forward) return true;
		FRHISampler* FallbackSampler = nullptr;
		if (OutMaterial.bCompiledLayout)
		{
			const auto* Entry = State->Samplers.Find(FMaterialSamplerState{});
			if (!Entry || HasSelectedRenderResourceGenerationChanged(Entry->Slot.GetPayloadGeneration(),
				Generation, ERenderResourceGenerationDependency::Device) || !Entry->Slot.GetPayload()) return false;
			FallbackSampler = Entry->Slot.GetPayload()->GetReference();
		}
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
			? EnvironmentSampler : FallbackSampler;
		OutMaterial.DirectionalShadowTexture = DirectionalShadowTexture != nullptr
			? DirectionalShadowTexture : DefaultTextures.GetArray_RenderThread();
		OutMaterial.DirectionalShadowSampler = DirectionalShadowSampler != nullptr
			? DirectionalShadowSampler : FallbackSampler;
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

	auto BindCompiledSurfaceMaterial(
		FRHICommandListImmediate& CommandList, FRHIShader* Shader,
		const FShaderReflectionData& Reflection,
		const FResolvedSurfaceMaterial& Material,
		const FRHIUniformBufferRange& MaterialBuffer,
		const FRHIUniformBufferRange& Lighting) -> bool
	{
		if (!Shader || !Material.bCompiledLayout
			|| Material.CompiledTextures.size() != Material.CompiledSamplers.size()) return false;
		std::vector<FRHIShaderParameterResource> Resources;
		for (const auto& Binding : Reflection.ResourceBindings)
		{
			if (Binding.SetIndex != 0 || Binding.ArraySize != 1) return false;
			FRHIShaderParameterResource Resource;
			Resource.BindingIndex = Binding.BindingIndex;
			Resource.Type = Binding.Type;
			ERHIBindingType Expected = ERHIBindingType::Texture;
			const uint32 Slot = Binding.BindingIndex;
			if (Slot == 1 || Slot == 2)
			{
				const auto& Range = Slot == 1 ? Lighting : MaterialBuffer;
				Resource.Resource = Range.Buffer;
				Resource.Offset = Range.Offset;
				Resource.Size = Range.Size;
				if (Range.Size == 0) return false;
				Expected = ERHIBindingType::UniformBuffer;
				Resource.Type = ERHIBindingType::UniformBufferDynamic;
			}
			else if (Slot >= MaterialTextureBindingBase)
			{
				const uint32 Index = (Slot - MaterialTextureBindingBase) / 2;
				if (Index >= Material.CompiledTextures.size()) return false;
				if ((Slot - MaterialTextureBindingBase) % 2 == 0) Resource.Resource = Material.CompiledTextures[Index];
				else { Resource.Resource = Material.CompiledSamplers[Index]; Expected = ERHIBindingType::Sampler; }
			}
			else switch (Slot)
			{
			case 19: Resource.Resource = Material.EnvironmentIrradiance; break;
			case 20: Resource.Resource = Material.EnvironmentPrefiltered; break;
			case 21: Resource.Resource = Material.EnvironmentBrdfLut; break;
			case 22: Resource.Resource = Material.EnvironmentSampler; Expected = ERHIBindingType::Sampler; break;
			case 25: Resource.Resource = Material.DirectionalShadowTexture; break;
			case 26: Resource.Resource = Material.DirectionalShadowSampler; Expected = ERHIBindingType::Sampler; break;
			default: return false;
			}
			if (!Resource.Resource || Binding.Type != Expected) return false;
			Resources.push_back(Resource);
		}
		CommandList.SetShaderParameters(Shader, Resources);
		return true;
	}

} // namespace Durin::RendererPrivate
