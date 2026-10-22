#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshRendererShared.h"
#include "DynamicRHI.h"
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
			FShaderMapPreparation FixedShaders;
			bool FixedShadersRequested = false;
			uint8 CompletedPasses = 0;
		};
		template<typename T> auto FragmentShader() -> FMaterialPSOFragmentShader
		{
			return {&T::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<T>(Map).GetRHIShader(false); }};
		}
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
		if (!Collection.Resources) Collection.Resources = std::make_shared<FCollectedMaterialPSOs>();
		auto Resources = std::static_pointer_cast<FCollectedMaterialPSOs>(Collection.Resources);
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
			Resources->FixedShaders = FShaderMapPreparation::Request("MeshVertexFactory", {});
			for (const auto Pass : {MaterialMeshPassForward, MaterialMeshPassShadow, MaterialMeshPassGBuffer})
			{
				const auto Needed = Pass == MaterialMeshPassShadow ? EMaterialPSOPrecachePass::ShadowDepth
					: Pass == MaterialMeshPassGBuffer ? EMaterialPSOPrecachePass::GBuffer
					: EMaterialPSOPrecachePass::Forward | EMaterialPSOPrecachePass::HybridRetainedForward
						| EMaterialPSOPrecachePass::SortedTranslucency | EMaterialPSOPrecachePass::HitProxy;
				if (!EnumHasAnyFlags(Options.Passes, Needed)) continue;
				if (!Factory->GetShaderType(Pass) || Factory->UsesGeneratedMaterialVertexStage(Pass)) continue;
				const auto Name = Factory->GetRuntimeRequestName(Pass);
				Resources->FixedShaders.Add("MeshVertexFactory", {Name});
			}
			if (EnumHasAnyFlags(Options.Passes, EMaterialPSOPrecachePass::ShadowDepth) && Blend != EMaterialBlendMode::Masked)
				Resources->FixedShaders.Add("SurfaceMaterial", {"Surface.OpaqueShadow"});
			if (EnumHasAnyFlags(Options.Passes, EMaterialPSOPrecachePass::HitProxy))
				Resources->FixedShaders.Add("HitProxy", {"Surface.HitProxy"});
			Resources->FixedShadersRequested = true;
		}
		const auto FixedReady = Resources->FixedShaders.Poll();
		if (!FixedReady) return std::unexpected(FixedReady.error());
		if (!*FixedReady) return false;
		const auto State = MakeMeshPipelineState(Params.Material,
			{Options.bWireframe, Options.bReverseCulling, Options.bOrientationChanging, Options.bReversedZ});
		FPreparedShaderMapScope Prepared;
		for (const auto Pass : {EMaterialPSOPrecachePass::Forward, EMaterialPSOPrecachePass::ShadowDepth, EMaterialPSOPrecachePass::GBuffer,
			EMaterialPSOPrecachePass::HybridRetainedForward, EMaterialPSOPrecachePass::SortedTranslucency, EMaterialPSOPrecachePass::HitProxy})
		{
			if (!EnumHasAnyFlags(Options.Passes, Pass) || (Resources->CompletedPasses & uint8(Pass))) continue;
			uint32 MeshPass = MaterialMeshPassForward;
			auto Route = EMeshPipelinePass::Forward;
			auto Fragment = FragmentShader<FSurfaceFragmentShader>();
			switch (Pass)
			{
			case EMaterialPSOPrecachePass::ShadowDepth:
				if (Blend == EMaterialBlendMode::Translucent) return std::unexpected(std::string("Translucent material has no requested shadow route."));
				MeshPass = MaterialMeshPassShadow; Route = EMeshPipelinePass::ShadowDepth;
				Fragment = Blend == EMaterialBlendMode::Masked ? FragmentShader<FSurfaceMaskedShadowFragmentShader>() : FragmentShader<FSurfaceOpaqueShadowFragmentShader>();
				break;
			case EMaterialPSOPrecachePass::GBuffer:
				if (Blend == EMaterialBlendMode::Translucent || Params.Material.ShaderMap.ShadingModel != EMaterialShadingModel::Lit)
					return std::unexpected(std::string("Material does not support the requested GBuffer route."));
				MeshPass = MaterialMeshPassGBuffer; Route = EMeshPipelinePass::GBuffer; Fragment = GetGBufferPSOPrecacheFragmentShader(); break;
			case EMaterialPSOPrecachePass::HybridRetainedForward: Route = EMeshPipelinePass::HybridRetainedForward; break;
			case EMaterialPSOPrecachePass::SortedTranslucency:
				if (Blend != EMaterialBlendMode::Translucent) return std::unexpected(std::string("Sorted translucency requires a translucent material."));
				Route = EMeshPipelinePass::SortedTranslucency; break;
			case EMaterialPSOPrecachePass::HitProxy: Route = EMeshPipelinePass::HitProxy; Fragment = GetHitProxyPSOPrecacheFragmentShader(); break;
			default: break;
			}
			const auto* VertexType = Factory->GetShaderType(MeshPass);
			if (!VertexType || !Fragment.Type || !Fragment.Resolve) return std::unexpected(std::string("Unsupported material precache mesh pass."));
			FMeshMaterialShaderMap Map;
			const auto Completion = InitializeMaterialShaderMap(*VertexType, *Fragment.Type, Factory->GetType(), MeshPass,
				Params.Material.ShaderMap, Generation, Params.CompiledProgram.get(), {}, Map);
			if (FPreparedShaderMapScope::GetPendingRequestCount()) return false;
			if (!Completion) return std::unexpected(Completion.Error.empty()
				? std::string("Material shader collection was canceled.") : std::string(Completion.Error));
			const auto Vertex = Factory->Resolve(Map, MeshPass);
			auto* VertexRHI = Vertex ? Vertex->GetRHIShader(false) : nullptr;
			auto* FragmentRHI = Fragment.Resolve(Map);
			if (!VertexRHI || !FragmentRHI) return std::unexpected(std::string("Precache material shader creation failed."));
			const auto Description = DescribeMeshPipeline(Route, State, Options.Topology, Blend, Options.bReversedZ);
			const auto Initializer = Description.MakeInitializer(VertexRHI, FragmentRHI, Resources->Declaration, Map.GetPipelineLayout());
			const auto Pipeline = PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, "MaterialPSOPrecache");
			if (!Pipeline)
			{
				if (Pipeline.error() == ERHIPipelineRequestRejection::CapacityExceeded) return false;
				return std::unexpected(std::string("Material PSO pipeline admission failed."));
			}
			Collection.Pipelines.push_back({*Pipeline, EnumHasAnyFlags(Options.RequiredPasses, Pass)});
			Resources->ShaderMaps.push_back(std::move(Map));
			Resources->CompletedPasses |= uint8(Pass);
		}
		return true;
	}
}
