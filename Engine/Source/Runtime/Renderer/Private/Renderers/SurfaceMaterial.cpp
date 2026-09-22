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
		FShaderRequestRegistration Registration;
		(void)RegisterShaderRuntimeRequest({.Category = EShaderRuntimeRequestCategory::FeatureProgram,
			.Owner = "SurfaceMaterial", .Name = "Surface.OpaqueShadow"},
			EShaderRequestEligibility::GameAndEditor, Registration, std::span(&Type, 1));
		return Registration;
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
				ERenderResourceCreateErrorReason::SamplerCreationFailed,
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
		std::vector<FMaterialSamplerState> RequiredSamplers(Binding.CompiledSamplers.begin(), Binding.CompiledSamplers.end());
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
				|| Binding.CompiledUniformPayload.size() < MaterialUniformHeaderBytes) return false;
			OutMaterial.PublishedBinding = Binding;
			OutMaterial.CompiledUniformPayload = Binding.CompiledUniformPayload;
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

	FCompiledSurfaceBindingLayout::FCompiledSurfaceBindingLayout(const FShaderReflectionData& Reflection)
	{
		Entries.reserve(Reflection.ResourceBindings.size());
		for (const auto& Binding : Reflection.ResourceBindings)
		{
			if (Binding.SetIndex > 1 || Binding.ArraySize != 1) return;
			if (std::ranges::any_of(Entries, [&](const FEntry& Entry) {
				return Entry.SetIndex == Binding.SetIndex && Entry.BindingIndex == Binding.BindingIndex;
			})) return;
			FEntry Entry{Binding.SetIndex, Binding.BindingIndex, Binding.Type};
			const uint32 Slot = Binding.BindingIndex;
			const bool bView = Binding.SetIndex == 0 && Slot == 0;
			const bool bMaterialSet = Slot == 2 || Slot == 27 || Slot >= MaterialTextureBindingBase;
			if (Binding.SetIndex != (bMaterialSet ? 1u : 0u)) return;
			ERHIBindingType Expected = ERHIBindingType::Texture;
			if (bView || Slot == 1 || Slot == 2 || Slot == 27)
			{
				Entry.Source = bView ? ESource::View : Slot == 27 ? ESource::HitProxy
					: Slot == 1 ? ESource::Lighting : ESource::Material;
				Expected = ERHIBindingType::UniformBuffer;
				Entry.Type = ERHIBindingType::UniformBufferDynamic;
			}
			else if (Slot >= MaterialTextureBindingBase)
			{
				Entry.ResourceIndex = (Slot - MaterialTextureBindingBase) / 2;
				const bool bSampler = (Slot - MaterialTextureBindingBase) % 2 != 0;
				Entry.Source = bSampler ? ESource::Sampler : ESource::Texture;
				Expected = bSampler ? ERHIBindingType::Sampler : ERHIBindingType::Texture;
			}
			else switch (Slot)
			{
			case 19: Entry.Source = ESource::EnvironmentIrradiance; break;
			case 20: Entry.Source = ESource::EnvironmentPrefiltered; break;
			case 21: Entry.Source = ESource::EnvironmentBrdfLut; break;
			case 22: Entry.Source = ESource::EnvironmentSampler; Expected = ERHIBindingType::Sampler; break;
			case 25: Entry.Source = ESource::DirectionalShadowTexture; break;
			case 26: Entry.Source = ESource::DirectionalShadowSampler; Expected = ERHIBindingType::Sampler; break;
			default: return;
			}
			if (Binding.Type != Expected) return;
			Entries.push_back(Entry);
		}
		bValid = true;
	}

	auto PrepareCompiledSurfaceMaterial(FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
		const FResolvedSurfaceMaterial& Material, const FRHIUniformBufferRange& MaterialBuffer,
		const FRHIUniformBufferRange& Lighting, const FRHIUniformBufferRange& HitProxy,
		const FRHIUniformBufferRange& View, FPreparedSurfaceMaterialBindings& OutBindings) -> bool
	{
		OutBindings = {};
		if (!Shader || !Layout.IsValid() || !Material.bCompiledLayout
			|| Material.CompiledTextures.size() != Material.CompiledSamplers.size()) return false;
		std::vector<FRHIShaderParameterResource> Resources;
		Resources.reserve(Layout.GetEntries().size());
		using ESource = FCompiledSurfaceBindingLayout::ESource;
		for (const auto& Entry : Layout.GetEntries())
		{
			FRHIShaderParameterResource Resource;
			Resource.SetIndex = Entry.SetIndex;
			Resource.BindingIndex = Entry.BindingIndex;
			Resource.Type = Entry.Type;
			const FRHIUniformBufferRange* Range = nullptr;
			switch (Entry.Source)
			{
			case ESource::View: Range = &View; break;
			case ESource::Lighting: Range = &Lighting; break;
			case ESource::Material: Range = &MaterialBuffer; break;
			case ESource::HitProxy: Range = &HitProxy; break;
			case ESource::Texture:
				if (Entry.ResourceIndex >= Material.CompiledTextures.size()) return false;
				Resource.Resource = Material.CompiledTextures[Entry.ResourceIndex]; break;
			case ESource::Sampler:
				if (Entry.ResourceIndex >= Material.CompiledSamplers.size()) return false;
				Resource.Resource = Material.CompiledSamplers[Entry.ResourceIndex]; break;
			case ESource::EnvironmentIrradiance: Resource.Resource = Material.EnvironmentIrradiance; break;
			case ESource::EnvironmentPrefiltered: Resource.Resource = Material.EnvironmentPrefiltered; break;
			case ESource::EnvironmentBrdfLut: Resource.Resource = Material.EnvironmentBrdfLut; break;
			case ESource::EnvironmentSampler: Resource.Resource = Material.EnvironmentSampler; break;
			case ESource::DirectionalShadowTexture: Resource.Resource = Material.DirectionalShadowTexture; break;
			case ESource::DirectionalShadowSampler: Resource.Resource = Material.DirectionalShadowSampler; break;
			}
			if (Range)
			{
				if (!Range->Buffer || Range->Size == 0 || Range->Offset > Range->Buffer->GetSize()
					|| Range->Size > Range->Buffer->GetSize() - Range->Offset) return false;
				Resource.Resource = Range->Buffer;
				Resource.Offset = Range->Offset;
				Resource.Size = Range->Size;
			}
			if (!Resource.Resource) return false;
			Resources.push_back(Resource);
		}
		OutBindings.Batch = FRHIShaderParameterBatch::Create(Shader, Resources);
		return OutBindings.Batch != nullptr;
	}

	auto FPreparedSurfaceMaterialBindings::Bind(FRHICommandList& CommandList) const -> bool
	{
		if (!Batch) return false;
		CommandList.SetPreparedShaderParameters(Batch);
		return true;
	}

} // namespace Durin::RendererPrivate
