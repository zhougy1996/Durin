#include "Renderers/SurfaceMaterial.h"
#include "Profiling/Profiling.h"
#include "Shader/ShaderCookedLibrary.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "RenderingThread.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"

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
			return FResult::Success(std::move(Candidate));
		}
	}

	struct FSurfaceMaterialResources::FState
	{
		struct FSamplerKeyHash
		{
			auto operator()(const FMaterialSamplerState& Key) const -> size_t
			{
				return static_cast<size_t>(Key.MinFilter)
					| (static_cast<size_t>(Key.MagFilter) << 8)
					| (static_cast<size_t>(Key.AddressU) << 16)
					| (static_cast<size_t>(Key.AddressV) << 24);
			}
		};
		TRendererResourceSlotCache<FMaterialSamplerState, FSamplerRHIRef, FSamplerKeyHash> Samplers{
			ERenderResourceGenerationDependency::Device};
		FSurfaceMaterialResourceCounters Counters;
		uint64 ResourceRevision = 0;
	};

	FSurfaceMaterialResources::FSurfaceMaterialResources(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, State(std::make_unique<FState>())
	{
	}

	FSurfaceMaterialResources::~FSurfaceMaterialResources() = default;

	auto FSurfaceMaterialResources::Ensure_RenderThread(
		const FMaterialRenderBinding& Binding,
		ESurfaceMaterialPass Pass) -> bool
	{
		check(IsInRenderingThread());
		if (Binding.bError) return true;
		if (Binding.LayoutIdentity.Version != CompiledMaterialRenderLayoutVersion) return false;
		std::vector<FMaterialSamplerState> RequiredSamplers(Binding.CompiledSamplers.begin(), Binding.CompiledSamplers.end());
		if (Pass == ESurfaceMaterialPass::Forward) RequiredSamplers.emplace_back();
		for (size_t Role = 0; Role < RequiredSamplers.size(); ++Role)
		{
			++State->Counters.SamplerLookups;
			const FMaterialSamplerState StateKey = RequiredSamplers[Role];
			const auto PreviousEvictions = State->Samplers.GetEvictions();
			auto& Entry = State->Samplers.FindOrAdd(StateKey);
			bool bCreationAttempted = false;
			auto* Sampler = Entry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[StateKey, &bCreationAttempted] {
					bCreationAttempted = true;
					return CreateSurfaceSampler(StateKey);
				},
				ReportRendererResourceCreateDiagnostic);
			if (bCreationAttempted || PreviousEvictions != State->Samplers.GetEvictions()) ++State->ResourceRevision;
			if (Sampler == nullptr)
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
		const std::shared_ptr<const FSkyLightingGeneration>& Environment,
		FRHITexture* DirectionalShadowTexture,
		FRHISampler* DirectionalShadowSampler,
		FResolvedSurfaceMaterial& OutMaterial) const -> bool
	{
		check(IsInRenderingThread());
		OutMaterial = {};

		const FRenderResourceGeneration Generation =
			Coordinator.GetGeneration_RenderThread();
		OutMaterial.bError = Binding.bError;
		OutMaterial.bCompiledLayout = Binding.LayoutIdentity.Version == CompiledMaterialRenderLayoutVersion;
		if (OutMaterial.bCompiledLayout)
		{
			const size_t Count = Binding.CompiledTextures.size();
			if (Binding.CompiledSamplers.size() != Count || Binding.CompiledTextureFallbacks.size() != Count
				|| Binding.CompiledUniformPayload.size() < MaterialUniformHeaderBytes) return false;
			if (Binding.bError) return Count == 0;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const auto* Entry = State->Samplers.Find(Binding.CompiledSamplers[Index]);
				if (!Entry || HasSelectedRenderResourceGenerationChanged(Entry->Slot.GetPayloadGeneration(),
					Generation, ERenderResourceGenerationDependency::Device) || !Entry->Slot.GetPayload()) return false;
				FRHITexture* Texture = Binding.CompiledTextures[Index];
				EDefaultTexture Fallback;
				switch (Binding.CompiledTextureFallbacks[Index])
				{
				case EMaterialTextureFallback::White: Fallback = EDefaultTexture::White; break;
				case EMaterialTextureFallback::Black: Fallback = EDefaultTexture::Black; break;
				case EMaterialTextureFallback::FlatRGNormal: Fallback = EDefaultTexture::FlatNormal; break;
				default: return false;
				}
				FRHITexture* FallbackTexture = DefaultTextures.Get_RenderThread(Fallback);
				if (!Texture) Texture = FallbackTexture;
				OutMaterial.CompiledTextureFallbacks.push_back(FallbackTexture);
				FRHISampler* Sampler = Entry->Slot.GetPayload()->GetReference();
				if (!Texture || !Sampler) return false;
				OutMaterial.CompiledTextures.push_back(Texture);
				OutMaterial.CompiledSamplers.push_back(Sampler);
			}
		}
		else return false;

		return RefreshPassResources_RenderThread(Pass, Environment, DirectionalShadowTexture,
			DirectionalShadowSampler, OutMaterial);
	}

	auto FSurfaceMaterialResources::RefreshPassResources_RenderThread(
		ESurfaceMaterialPass Pass, const std::shared_ptr<const FSkyLightingGeneration>& Environment, FRHITexture* DirectionalShadowTexture,
		FRHISampler* DirectionalShadowSampler, FResolvedSurfaceMaterial& OutMaterial) const -> bool
	{
		check(IsInRenderingThread());
		if (Pass != ESurfaceMaterialPass::Forward || OutMaterial.bError) return true;
		const auto Generation = Coordinator.GetGeneration_RenderThread();
		FRHISampler* FallbackSampler = nullptr;
		if (OutMaterial.bCompiledLayout)
		{
			const auto* Entry = State->Samplers.Find(FMaterialSamplerState{});
			if (!Entry || HasSelectedRenderResourceGenerationChanged(Entry->Slot.GetPayloadGeneration(),
				Generation, ERenderResourceGenerationDependency::Device) || !Entry->Slot.GetPayload()) return false;
			FallbackSampler = Entry->Slot.GetPayload()->GetReference();
		}
		FRHITexture* Irradiance = (Environment ? Environment->Irradiance.GetReference() : nullptr);
		FRHITexture* Prefiltered = (Environment ? Environment->Prefiltered.GetReference() : nullptr);
		FRHITexture* Brdf = (Environment ? Environment->BrdfLut.GetReference() : nullptr);
		FRHISampler* EnvironmentSampler = (Environment ? Environment->Sampler.GetReference() : nullptr);
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
		++State->ResourceRevision;
		State->Samplers.Reset();
		State->Counters = {};
	}

	// Revision covers payload creation/replacement, bounded eviction and reset.
	// A successful Ensure result is reusable only while this revision is current.
	auto FSurfaceMaterialResources::GetResourceRevision_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		return State->ResourceRevision;
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
			const bool bPrimitive = Binding.SetIndex == 1 && Slot == 0;
			const bool bCollection = Slot >= 3
				&& Slot < 3 + MaterialParameterCollectionMaxPerMaterial;
			const bool bScene = Binding.SetIndex == 0 && (Slot == 27 || Slot == 28 || Slot == 29 || Slot == 31);
			const bool bMaterialSet = !bScene && (bPrimitive || Slot == 2 || Slot == 27 || bCollection
				|| Slot >= MaterialTextureBindingBase);
			if (Binding.SetIndex != (bMaterialSet ? 1u : 0u)) return;
			ERHIBindingType Expected = ERHIBindingType::Texture;
			if (bScene)
			{
				Entry.Source = Slot == 27 ? ESource::GPUScenePrimitive : Slot == 28 ? ESource::GPUSceneInstance : Slot == 29 ? ESource::GPUScenePayload : ESource::GPUSceneInstanceIds;
				Expected = ERHIBindingType::StorageBuffer;
			}
			else if (bView || bPrimitive || Slot == 1 || Slot == 2 || Slot == 27)
			{
				Entry.Source = bView ? ESource::View : bPrimitive ? ESource::Primitive : Slot == 27 ? ESource::HitProxy
					: Slot == 1 ? ESource::Lighting : ESource::Material;
				Expected = ERHIBindingType::UniformBuffer;
				Entry.Type = ERHIBindingType::UniformBufferDynamic;
			}
			else if (bCollection)
			{
				Entry.Source = ESource::Collection;
				Entry.ResourceIndex = Slot - 3;
				Expected = ERHIBindingType::UniformBuffer;
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
			case 30: Entry.Source = ESource::LocalLights; Expected = ERHIBindingType::StorageBuffer; break;
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
		const FRHIUniformBufferRange& View, const FRHIUniformBufferRange& Primitive,
		std::span<const FRHIUniformBufferRange> Collections,
		FMeshDrawShaderBindings& OutBindings, FRHIStorageBufferRange LocalLights, const FGPUSceneShaderParameters& GPUScene) -> bool
	{
		OutBindings = {};
		if (!Shader || !Layout.IsValid() || !Material.bCompiledLayout
			|| Material.CompiledTextures.size() != Material.CompiledSamplers.size()) return false;
		std::vector<FRHIShaderParameterResource> Resources;
		Resources.reserve(Layout.GetEntries().size());
		using ESource = FCompiledSurfaceBindingLayout::ESource;
		const auto* Uniform = MaterialBuffer.Buffer && MaterialBuffer.Buffer->GetResourceType() == ERHIResourceType::UniformBuffer
			? static_cast<const FRHIUniformBuffer*>(MaterialBuffer.Buffer) : nullptr;
		auto BindMember = [&](FRHIShaderParameterResource& Resource, uint32 Index) {
			if (!Uniform || Index >= Uniform->GetLayout().Resources.size()) return false;
			Resource.Resource = MaterialBuffer.Buffer; Resource.ResourceIndex = Index; return true;
		};
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
			case ESource::Primitive: Range = &Primitive; break;
			case ESource::LocalLights:
				Resource.Resource = LocalLights.Buffer;
				Resource.Offset = LocalLights.Offset;
				Resource.Size = LocalLights.Size;
				break;
			case ESource::GPUScenePrimitive:
            case ESource::GPUSceneInstance:
            case ESource::GPUScenePayload:
            case ESource::GPUSceneInstanceIds:
            {
                const auto& Data = Entry.Source == ESource::GPUScenePrimitive ? GPUScene.GPUScenePrimitiveSceneData
                    : Entry.Source == ESource::GPUSceneInstance ? GPUScene.GPUSceneInstanceSceneData
                    : Entry.Source == ESource::GPUScenePayload ? GPUScene.GPUSceneInstancePayloadData : GPUScene.GPUSceneInstanceIds;
                Resource.Resource = Data.Buffer; Resource.Offset = Data.Offset; Resource.Size = Data.Size;
                break;
            }
            case ESource::Lighting: Range = &Lighting; break;
			case ESource::Material: Range = &MaterialBuffer; break;
			case ESource::HitProxy: Range = &HitProxy; break;
			case ESource::Collection:
				if (Entry.ResourceIndex >= Collections.size()) return false;
				Range = &Collections[Entry.ResourceIndex];
				if (BindMember(Resource, static_cast<uint32>(Material.CompiledTextures.size() * 3) + Entry.ResourceIndex)) Range = nullptr;
				break;
			case ESource::Texture:
				if (Entry.ResourceIndex >= Material.CompiledTextures.size()) return false;
				Resource.Resource = Material.CompiledTextures[Entry.ResourceIndex];
				if (!BindMember(Resource, Entry.ResourceIndex * 3) && Resource.Resource && Resource.Resource->GetResourceType() == ERHIResourceType::TextureReference)
				{
					Resource.Resource = static_cast<FRHITextureReference*>(Resource.Resource)->GetReferencedTexture_RenderThread();
					if (!Resource.Resource && Entry.ResourceIndex < Material.CompiledTextureFallbacks.size()) Resource.Resource = Material.CompiledTextureFallbacks[Entry.ResourceIndex];
				}
				break;
			case ESource::Sampler:
				if (Entry.ResourceIndex >= Material.CompiledSamplers.size()) return false;
				Resource.Resource = Material.CompiledSamplers[Entry.ResourceIndex];
				BindMember(Resource, Entry.ResourceIndex * 3 + 1);
				break;
			case ESource::EnvironmentIrradiance: Resource.Resource = Material.EnvironmentIrradiance; break;
			case ESource::EnvironmentPrefiltered: Resource.Resource = Material.EnvironmentPrefiltered; break;
			case ESource::EnvironmentBrdfLut: Resource.Resource = Material.EnvironmentBrdfLut; break;
			case ESource::EnvironmentSampler: Resource.Resource = Material.EnvironmentSampler; break;
			case ESource::DirectionalShadowTexture: Resource.Resource = Material.DirectionalShadowTexture; break;
			case ESource::DirectionalShadowSampler: Resource.Resource = Material.DirectionalShadowSampler; break;
			}
			if (Range)
			{
				if (!Range->Buffer || Range->Size == 0) return false;
				uint32 BufferSize = 0;
				if (Range->Buffer->GetResourceType() == ERHIResourceType::UniformBuffer)
					BufferSize = static_cast<const FRHIUniformBuffer*>(Range->Buffer)->GetSize();
				else if (IsBufferResource(Range->Buffer))
					BufferSize = static_cast<const FRHIBuffer*>(Range->Buffer)->GetSize();
				else return false;
				if (Range->Offset > BufferSize || Range->Size > BufferSize - Range->Offset) return false;
				Resource.Resource = Range->Buffer;
				Resource.Offset = Range->Offset;
				Resource.Size = Range->Size;
			}
			if (!Resource.Resource) return false;
			Resources.push_back(Resource);
		}
		OutBindings.Shader = Shader; OutBindings.Parameters = std::move(Resources);
		return true;
	}

	auto PrepareCompiledSurfaceMaterial(FRHIShader* Shader,
		const FCompiledSurfaceBindingLayout& Layout,
		const FResolvedSurfaceMaterial& Material,
		const FRHIUniformBufferRange& MaterialBuffer,
		const FRHIUniformBufferRange& Lighting,
		const FRHIUniformBufferRange& HitProxy,
		const FRHIUniformBufferRange& View,
		const FRHIUniformBufferRange& Primitive,
		FMeshDrawShaderBindings& OutBindings) -> bool
	{
		return PrepareCompiledSurfaceMaterial(Shader, Layout, Material,
			MaterialBuffer, Lighting, HitProxy, View, Primitive, {}, OutBindings);
	}

} // namespace Durin::RendererPrivate

namespace Durin::RendererPrivate
{
	namespace
	{
		auto CollectMaterialUniformResources(const FResolvedSurfaceMaterial& Surface,
			std::span<const FRHIUniformBufferRange> Collections, std::vector<FRHIResource*>& Resources,
			std::vector<FRHIUniformBufferResource>& Members) -> void
		{
			for (uint32 Index = 0; Index < Surface.CompiledTextures.size(); ++Index)
			{
				const uint32 FallbackIndex = static_cast<uint32>(Resources.size()) + 2;
				Resources.insert(Resources.end(), {Surface.CompiledTextures[Index], Surface.CompiledSamplers[Index], Surface.CompiledTextureFallbacks[Index]});
				Members.insert(Members.end(), {{ERHIBindingType::Texture, FallbackIndex}, {ERHIBindingType::Sampler}, {ERHIBindingType::Texture}});
			}
			for (const auto& Collection : Collections)
			{ require(Collection.Offset == 0); Resources.push_back(Collection.Buffer); Members.push_back({ERHIBindingType::UniformBuffer}); }
		}
	}
	auto FSurfaceMaterialResources::UpdateUniformExpressionCache_RenderThread(FRHICommandListImmediate& Commands,
		FUniformExpressionCache& Cache, const FMaterialRenderRepresentation& Representation,
		const FMaterialRenderBinding& Binding, std::span<const FRHIUniformBufferRange> Collections) const -> bool
	{
		const auto* Uniform = Cache.UniformBuffer->GetRHI();
		const size_t Base = Representation.GetResources().size() * 3;
		if (Cache.IsUpToDate(Representation) && Uniform->GetResourceTable().size() == Base + Collections.size())
		{
			const auto Table = Uniform->GetResourceTable();
			bool Current = true;
			for (size_t Index = 0; Index < Collections.size(); ++Index) Current &= Table[Base + Index].GetReference() == Collections[Index].Buffer;
			if (Current) return true;
		}
		FResolvedSurfaceMaterial Surface;
		if (!Resolve_RenderThread(Binding, ESurfaceMaterialPass::GBuffer, {}, nullptr, nullptr, Surface)) return false;
		std::vector<FRHIResource*> Resources;
		std::vector<FRHIUniformBufferResource> Members;
		CollectMaterialUniformResources(Surface, Collections, Resources, Members);
		return Cache.UpdateUniformBuffer(Commands, Representation, Resources, Members);
	}
	auto FSurfaceMaterialResources::Prepare_RenderThread(FRHICommandListImmediate& Commands, const FMaterialRenderBinding* Binding,
		ESurfaceMaterialPass Pass, const std::shared_ptr<const FSkyLightingGeneration>& Environment,
		FRHITexture* DirectionalShadowTexture, FRHISampler* DirectionalShadowSampler,
		FPreparedSurfaceMaterial& OutMaterial, const FRHIUniformBufferRange& SharedUniform) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareSurfaceMaterial");
		if (Binding == nullptr || !Resolve_RenderThread(
			*Binding, Pass, Environment,
			DirectionalShadowTexture, DirectionalShadowSampler,
			OutMaterial.Surface))
		{
			return false;
		}
		if (OutMaterial.Surface.bCompiledLayout)
		{
			OutMaterial.Uniform = SharedUniform;
			if (!OutMaterial.Uniform.Buffer)
			{
				std::vector<FRHIResource*> Resources;
				std::vector<FRHIUniformBufferResource> Members;
				CollectMaterialUniformResources(OutMaterial.Surface, {}, Resources, Members);
				const auto Buffer = Commands.CreateUniformBuffer({static_cast<uint32>(Binding->CompiledUniformPayload.size()), std::move(Members)},
					ERHIBufferLifetimeUsage::SingleFrame, Binding->CompiledUniformPayload, Resources);
				OutMaterial.Uniform = {Buffer.GetReference(), 0, static_cast<uint32>(Binding->CompiledUniformPayload.size()), Buffer};
			}
			return OutMaterial.Uniform.Buffer != nullptr;
		}
		return false;
	}

}
