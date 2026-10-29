#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshRendererShared.h"
#include "Renderers/MeshVertexFactory.h"
#include "DynamicRHI.h"
#include "RHICapabilities.h"
#include "Renderers/MeshPassRelevance.h"
#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderMapPreparation.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		struct FCollectedMaterialPSOs
		{
			FVertexDeclarationRHIRef Declaration;
			std::vector<FMeshMaterialShaderMap> ShaderMaps;
			std::array<FShaderMapPreparation, 2> FixedShaders;
			bool FixedShadersRequested = false;
			std::vector<FMaterialPSOPrecachePass> Passes;
			size_t NextPass = 0;
		};
		template<typename T> auto FragmentShader() -> FMaterialPSOFragmentShader
		{
			return {&T::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<T>(Map).GetRHIShader(false); }};
		}
	}
	auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
		bool bSupportsGBuffer, const FMaterialPSOPrecacheConfig& Config) -> std::vector<FMaterialPSOPrecachePass>
	{
		std::vector<FMaterialPSOPrecachePass> Result;
		const auto& Options = Params.PrecachePSOParams;
		for (const auto Pass : {EMeshPipelinePass::Forward, EMeshPipelinePass::ShadowDepth,
			EMeshPipelinePass::GBuffer, EMeshPipelinePass::HybridRetainedForward,
			EMeshPipelinePass::SortedTranslucency, EMeshPipelinePass::HitProxy})
		{
			if ((Pass == EMeshPipelinePass::Forward && !Config.bForward)
				|| ((Pass == EMeshPipelinePass::GBuffer || Pass == EMeshPipelinePass::HybridRetainedForward
					|| Pass == EMeshPipelinePass::SortedTranslucency) && !Config.bHybrid)
				|| (Pass == EMeshPipelinePass::HitProxy && !Config.bHitProxy)) continue;
			if (!IsMeshPassRelevant(Pass, Params.Material, Options.Participation, bSupportsGBuffer)) continue;
			// Shadows always draw solid with forward depth; picking always draws solid.
			// Enumerate their effective states so irrelevant view modes cannot demote
			// shadow requirements or generate duplicate normalized descriptions.
			const bool bShadow = Pass == EMeshPipelinePass::ShadowDepth;
			const uint8 Variants = bShadow ? 2 : Pass == EMeshPipelinePass::HitProxy ? 4 : 8;
			for (uint8 Variant = 0; Variant < Variants; ++Variant)
			{
				const FMeshPipelineRasterOptions Raster{(Variant & 4) != 0, (Variant & 1) != 0,
					Options.bOrientationChanging, (Variant & 2) != 0};
				if (Raster.bWireframe && !Config.bSupportsWireframe) continue;
				Result.push_back({Pass, Raster, bShadow || (Pass != EMeshPipelinePass::HitProxy
					&& Raster.bWireframe == Options.bWireframe && Raster.bReversedZ == Options.bReversedZ)});
			}
		}
		std::stable_sort(Result.begin(), Result.end(), [](const auto& A, const auto& B) { return A.bRequired > B.bRequired; });
		return Result;
	}
	auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
		const FMeshVertexFactoryImplementation& Factory, const FMaterialPSOPrecacheConfig& Config)
		-> std::vector<FMaterialPSOPrecachePass>
	{
		auto Result = SelectMaterialPSOPrecachePasses(Params, Factory.GetShaderType(MaterialMeshPassGBuffer) != nullptr, Config);
		// Receiver collection requires Forward support even for GBuffer draws.
		// A missing shadow shader excludes casting; it must not reject valid receivers.
		std::erase_if(Result, [&](const auto& Entry) {
			return !Factory.GetShaderType(Entry.Pass == EMeshPipelinePass::ShadowDepth
				? MaterialMeshPassShadow : MaterialMeshPassForward);
		});
		return Result;
	}
	auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params, FMaterialPSOPrecacheCollection& Collection,
		const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>
	{
		CheckRenderingThread();
		if (!Params.CompiledProgram) return std::unexpected(std::string("Material PSO collection requires an accepted program."));
		if (!GDynamicRHI || !IsTaskSchedulerRunning()) return std::unexpected(std::string("Material PSO collection requires RHI and the task scheduler."));
		const auto Factory = FindMeshVertexFactory(Params.VertexFactoryData.VertexFactoryType->GetStableKey());
		if (!Factory || &Factory->GetType() != Params.VertexFactoryData.VertexFactoryType)
			return std::unexpected(std::string("No registered mesh shader implementation for the precache Vertex Factory."));
		if (!Collection.Resources)
		{
			auto Resources = std::make_shared<FCollectedMaterialPSOs>();
			FMaterialPSOPrecacheConfig Config;
			const auto* Caps = GDynamicRHI->RHIGetCapabilities();
			Config.bSupportsWireframe = Caps && Caps->bSupportsNonSolidFill;
			Resources->Passes = SelectMaterialPSOPrecachePasses(Params, *Factory, Config);
			Collection.Resources = std::move(Resources);
		}
		auto Resources = std::static_pointer_cast<FCollectedMaterialPSOs>(Collection.Resources);
		if (Resources->Passes.empty())
		{
			Collection.bRequiredPSOsCollected = true;
			return true;
		}
		if (!Resources->Declaration)
		{
			const auto Elements = Params.VertexFactoryData.GetVertexElements();
			if (!Elements) return std::unexpected(std::string("Invalid precache vertex elements."));
			Resources->Declaration = GDynamicRHI->RHICreateVertexDeclaration(*Elements);
			if (!Resources->Declaration) return std::unexpected(std::string("Precache vertex declaration creation failed."));
		}
		const auto Blend = Params.Material.ShaderMap.BlendMode;
		const auto& Options = Params.PrecachePSOParams;
		if (!Resources->FixedShadersRequested)
		{
			for (auto& Shaders : Resources->FixedShaders) Shaders = FShaderMapPreparation::Request("MeshVertexFactory", {});
			std::array<std::array<bool, 6>, 2> Requested{};
			for (const auto& Entry : Resources->Passes)
			{
				const auto Pass = Entry.Pass;
				auto& bRequested = Requested[Entry.bRequired ? 0 : 1][static_cast<size_t>(Pass)];
				if (std::exchange(bRequested, true)) continue;
				auto& Shaders = Resources->FixedShaders[Entry.bRequired ? 0 : 1];
				const auto MeshPass = Pass == EMeshPipelinePass::ShadowDepth ? MaterialMeshPassShadow
					: Pass == EMeshPipelinePass::GBuffer ? MaterialMeshPassGBuffer : MaterialMeshPassForward;
				if (Factory->GetShaderType(MeshPass) && !Factory->UsesGeneratedMaterialVertexStage(MeshPass))
					Shaders.Add("MeshVertexFactory", {Factory->GetRuntimeRequestName(MeshPass)});
				if (Pass == EMeshPipelinePass::ShadowDepth && Blend != EMaterialBlendMode::Masked)
					Shaders.Add("SurfaceMaterial", {"Surface.OpaqueShadow"});
				if (Pass == EMeshPipelinePass::HitProxy) Shaders.Add("HitProxy", {"Surface.HitProxy"});
			}
			Resources->FixedShadersRequested = true;
		}
		FPreparedShaderMapScope Prepared;
		for (uint32 Group = 0; Group < 2; ++Group)
		{
			if (Group == 1) Collection.bRequiredPSOsCollected = true;
			const auto FixedReady = Resources->FixedShaders[Group].Poll();
			if (!FixedReady) return std::unexpected(FixedReady.error());
			if (!*FixedReady) return false;
			while (Resources->NextPass < Resources->Passes.size())
			{
				const auto& Entry = Resources->Passes[Resources->NextPass];
				if (Entry.bRequired != (Group == 0)) break;
				const auto Pass = Entry.Pass;
				const auto State = MakeMeshPipelineState(Params.Material, Entry.Raster);
				uint32 MeshPass = MaterialMeshPassForward;
				auto Route = EMeshPipelinePass::Forward;
				auto Fragment = FragmentShader<FSurfaceFragmentShader>();
				switch (Pass)
				{
				case EMeshPipelinePass::ShadowDepth:
					MeshPass = MaterialMeshPassShadow; Route = EMeshPipelinePass::ShadowDepth;
					Fragment = Blend == EMaterialBlendMode::Masked ? FragmentShader<FSurfaceMaskedShadowFragmentShader>() : FragmentShader<FSurfaceOpaqueShadowFragmentShader>();
					break;
				case EMeshPipelinePass::GBuffer:
					MeshPass = MaterialMeshPassGBuffer; Route = EMeshPipelinePass::GBuffer; Fragment = GetGBufferPSOPrecacheFragmentShader(); break;
				case EMeshPipelinePass::HybridRetainedForward: Route = EMeshPipelinePass::HybridRetainedForward; break;
				case EMeshPipelinePass::SortedTranslucency:
					Route = EMeshPipelinePass::SortedTranslucency; break;
				case EMeshPipelinePass::HitProxy: Route = EMeshPipelinePass::HitProxy; Fragment = GetHitProxyPSOPrecacheFragmentShader(); break;
				default: break;
				}
				const auto* VertexType = Factory->GetShaderType(MeshPass);
				if (!VertexType || !Fragment.Type || !Fragment.Resolve) return std::unexpected(std::string("Unsupported material precache mesh pass."));
				FMeshMaterialShaderMap Map;
				const auto Completion = InitializeMaterialShaderMap(*VertexType, *Fragment.Type, Factory->GetType(), MeshPass,
					Params.Material.ShaderMap, Generation, Params.CompiledProgram.get(), {}, Map);
				if (Completion.Status == EShaderBuildStatus::Pending) return false;
				if (!Completion) return std::unexpected(Completion.Error.empty()
					? std::string("Material shader collection was canceled.") : std::string(Completion.Error));
				const auto DeferredBefore = FRenderResourceAdmissionScope::GetDeferredAdmissions();
				const auto Vertex = Factory->Resolve(Map, MeshPass);
				auto* VertexRHI = Vertex ? Vertex->GetRHIShader(false) : nullptr;
				auto* FragmentRHI = Fragment.Resolve(Map);
				if ((!VertexRHI || !FragmentRHI) && (FRenderResourceAdmissionScope::GetDeferredAdmissions() != DeferredBefore)) return false;
				if (!VertexRHI || !FragmentRHI) return std::unexpected(std::string("Precache material shader creation failed."));
				const auto Description = DescribeMeshPipeline(Route, State, Options.Topology, Blend, Entry.Raster.bReversedZ);
				const auto Initializer = Description.MakeInitializer(VertexRHI, FragmentRHI, Resources->Declaration, Map.GetPipelineLayout());
				Collection.PSOInitializers.emplace_back(Initializer,
					Entry.bRequired, Params.Priority);
				Resources->ShaderMaps.push_back(std::move(Map));
				++Resources->NextPass;
			}
		}
		return true;
	}
}
