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
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSkyBackgroundMaterialShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSingleLayerWaterFragmentShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSingleLayerWaterDepthFragmentShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader);
	DURIN_IMPLEMENT_MATERIAL_SHADER(FSurfaceOpaqueShadowFragmentShader);
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
					| (static_cast<size_t>(Key.AddressV) << 24)
					| (static_cast<size_t>(Key.AddressW) << 32);
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
			if (!Binding.CompiledLayout || Binding.CompiledLayout->Identity != Binding.LayoutIdentity || Binding.CompiledSamplers.size() != Count || Binding.CompiledTextureFallbacks.size() != Count
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
				const auto& Fields = Binding.CompiledLayout->Fields;
				const auto Field = std::ranges::find_if(Fields, [&](const auto& V) {
					return V.Storage == EMaterialRenderFieldStorage::Resource && V.CompactIndex == Index;
				});
				if (Field == Fields.end()) return false;
				const bool Cube = Field->Type == EMaterialRenderValueType::TextureCube;
				const auto* Actual = Binding.CompiledTextures[Index] ? Binding.CompiledTextures[Index]->GetReferencedTexture_RenderThread() : nullptr;
				if (Actual && Actual->GetDimension() != (Cube ? ETextureDimension::TextureCube : ETextureDimension::Texture2D)) return false;
				FRHITexture* FallbackTexture = Cube ? DefaultTextures.GetCube_RenderThread(Fallback) : DefaultTextures.Get_RenderThread(Fallback);
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

    FCompiledSurfaceBindingLayout::FCompiledSurfaceBindingLayout(std::span<const FMaterialShaderBinding> Bindings)
    {
        std::set<std::pair<uint32, uint32>> Seen;
        std::set<std::string> Names;
        const auto* Metadata = FSurfaceParameterProviders::GetParametersMetadata();
        for (const auto& B : Bindings)
        {
            if (B.Name.empty() || B.OwnerName.empty() || !B.ArraySize || B.ArraySize > 1024
                || B.Type > ERHIBindingType::StorageImage || B.RuntimeType > ERHIBindingType::StorageImage
                || (B.Type != B.RuntimeType && !(B.Type == ERHIBindingType::UniformBuffer && B.RuntimeType == ERHIBindingType::UniformBufferDynamic))
                || !Seen.emplace(B.SetIndex, B.BindingIndex).second || !Names.insert(B.Name).second) return;
            FEntry E; static_cast<FMaterialShaderBinding&>(E) = B;
            if (B.QualifiedName.starts_with("Material.Texture.")) E.Storage = EStorage::Texture;
            else if (B.QualifiedName.starts_with("Material.Sampler.")) E.Storage = EStorage::Sampler;
            else if (B.QualifiedName.starts_with("Material.Collection.")) E.Storage = EStorage::Collection;
            else {
                const auto M = std::ranges::find_if(Metadata->Members, [&](const auto& V) { return V.Name && B.Name == V.Name; });
                if (M != Metadata->Members.end()) E.StructureBinding = FShaderParameterBinding{
                    B.Name, M->Offset, B.SetIndex, B.BindingIndex, B.RuntimeType, B.ArraySize, false, B.OwnerName, B.Scope};
            }
            Entries.push_back(std::move(E));
        }
        bValid = true;
    }

    auto PrepareCompiledSurfaceMaterial(FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
        const FResolvedSurfaceMaterial& Material, const FRHIUniformBufferRange& MaterialBuffer,
        const FRHIUniformBufferRange& Lighting, const FRHIUniformBufferRange& HitProxy,
        const FRHIUniformBufferRange& View, const FRHIUniformBufferRange& Primitive,
        std::span<const FRHIUniformBufferRange> Collections,
        FMeshDrawShaderBindings& OutBindings, FRHIStorageBufferRange LocalLights, const FGPUSceneShaderParameters& GPUScene, const FRHIUniformBufferRange& Sky) -> bool
    {
        OutBindings = {};
        if (!Shader || !Layout.IsValid() || !Material.bCompiledLayout
            || Material.CompiledTextures.size() != Material.CompiledSamplers.size()) return false;
        FSurfaceParameterProviders::FParameters Values;
        Values.MaterialSky = Sky;
        Values.Material = MaterialBuffer; Values.MaterialView = View; Values.MaterialPrimitive = Primitive;
        Values.Lighting = Lighting; Values.HitProxy = HitProxy; Values.LocalLights = LocalLights;
        Values.GPUScenePrimitiveSceneData = GPUScene.GPUScenePrimitiveSceneData;
        Values.GPUSceneInstanceSceneData = GPUScene.GPUSceneInstanceSceneData;
        Values.GPUSceneInstancePayloadData = GPUScene.GPUSceneInstancePayloadData;
        Values.GPUSceneInstanceIds = GPUScene.GPUSceneInstanceIds;
        Values.EnvironmentIrradiance = Material.EnvironmentIrradiance;
        Values.EnvironmentPrefiltered = Material.EnvironmentPrefiltered;
        Values.EnvironmentBrdfLut = Material.EnvironmentBrdfLut;
        Values.EnvironmentSampler = Material.EnvironmentSampler;
        Values.DirectionalShadowTexture = Material.DirectionalShadowTexture;
        Values.DirectionalShadowSampler = Material.DirectionalShadowSampler;
        const auto* Uniform = MaterialBuffer.Buffer && MaterialBuffer.Buffer->GetResourceType() == ERHIResourceType::UniformBuffer
            ? static_cast<const FRHIUniformBuffer*>(MaterialBuffer.Buffer) : nullptr;
        std::vector<FRHIShaderParameterResource> Resources;
        std::optional<std::pair<uint32, uint32>> InstanceIds;
        auto Member = [&](FRHIShaderParameterResource& Resource, uint32 Index) {
            if (!Uniform || Index >= Uniform->GetLayout().Resources.size()) return false;
            Resource.Resource = MaterialBuffer.Buffer; Resource.ResourceIndex = Index; return true;
        };
        auto ValidUniformRange = [](FRHIResource* Resource, uint32 Offset, uint32 Size, uint32 Minimum) {
            if (!Resource || !Size || Size < Minimum) return false;
            const uint64 Capacity = Resource->GetResourceType() == ERHIResourceType::UniformBuffer
                ? static_cast<const FRHIUniformBuffer*>(Resource)->GetSize() : IsBufferResource(Resource)
                    ? static_cast<const FRHIBuffer*>(Resource)->GetSize() : 0;
            return Offset <= Capacity && Size <= Capacity - Offset;
        };
        for (const auto& E : Layout.GetEntries())
        {
            FRHIShaderParameterResource R;
            R.SetIndex = E.SetIndex; R.BindingIndex = E.BindingIndex; R.Type = E.RuntimeType;
            if (E.Storage == FCompiledSurfaceBindingLayout::EStorage::Structure) {
                if (!E.StructureBinding) return false;
                auto Resolved = ResolveShaderParameterResources(*FSurfaceParameterProviders::GetParametersMetadata(),
                    std::span(&*E.StructureBinding, 1), &Values);
                for (const auto& V : Resolved) {
                    if (!V.Resource) return false;
                    if ((V.Type == ERHIBindingType::UniformBuffer || V.Type == ERHIBindingType::UniformBufferDynamic)
                        && !ValidUniformRange(V.Resource, V.Offset, V.Size, E.ConstantBufferSize)) return false;
                    Resources.push_back(V);
                }
            } else {
                const uint32 I = E.ResourceIndex;
                if (E.ArraySize != 1) return false;
                if (E.Storage == FCompiledSurfaceBindingLayout::EStorage::Collection) {
                    if (I >= Collections.size()) return false;
                    const auto& Range = Collections[I];
                    if (!Member(R, static_cast<uint32>(Material.CompiledTextures.size() * 3) + I)) {
                        if (!ValidUniformRange(Range.Buffer, Range.Offset, Range.Size, E.ConstantBufferSize)) return false;
                        R.Resource = Range.Buffer; R.Offset = Range.Offset; R.Size = Range.Size;
                    }
                } else if (E.Storage == FCompiledSurfaceBindingLayout::EStorage::Texture) {
                    if (I >= Material.CompiledTextures.size()) return false;
                    R.Resource = Material.CompiledTextures[I];
                    if (!Member(R, I * 3) && R.Resource && R.Resource->GetResourceType() == ERHIResourceType::TextureReference) {
                        R.Resource = static_cast<FRHITextureReference*>(R.Resource)->GetReferencedTexture_RenderThread();
                        if (!R.Resource && I < Material.CompiledTextureFallbacks.size()) R.Resource = Material.CompiledTextureFallbacks[I];
                    }
                } else {
                    if (I >= Material.CompiledSamplers.size()) return false;
                    R.Resource = Material.CompiledSamplers[I]; Member(R, I * 3 + 1);
                }
                if (!R.Resource) return false;
                Resources.push_back(R);
            }
            if (E.Name == "GPUSceneInstanceIds") InstanceIds = std::pair{E.SetIndex, E.BindingIndex};
        }
        OutBindings.Shader = Shader; OutBindings.Parameters = std::move(Resources); OutBindings.InstanceIdsAllocation = InstanceIds;
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
