#include "Renderers/MeshPassProcessor.h"

#include "HitProxy.h"
#include "Renderers/MeshRendererShared.h"
#include "Resources/RenderTargetLayouts.h"

namespace Durin::RendererPrivate
{
	namespace
	{
		template<typename T> auto FragmentShader() -> FMeshPassFragmentShader
		{
			return {&T::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<T>(Map).GetRHIShader(false); }};
		}
		auto RasterVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params,
			uint8 Count, bool bShadow = false, bool bPicking = false) -> std::vector<FMeshPSOPrecacheVariant>
		{
			std::vector<FMeshPSOPrecacheVariant> Result;
			for (uint8 Variant = 0; Variant < Count; ++Variant)
			{
				const FMeshPipelineRasterOptions Raster{(Variant & 4) != 0, (Variant & 1) != 0,
					Params.bOrientationChanging, (Variant & 2) != 0};
				if (Raster.bWireframe && !Config.bSupportsWireframe) continue;
				Result.push_back({Raster, bShadow || (!bPicking
					&& Raster.bWireframe == Params.bWireframe && Raster.bReversedZ == Params.bReversedZ)});
			}
			return Result;
		}
		template<typename T> class TMeshPassProcessor : public FMeshPassProcessor
		{
		public:
			using FMeshPassProcessor::FMeshPassProcessor;
			auto CreatePSOCollector(FMeshPSOCollectorContext Context) const -> std::unique_ptr<FMeshPassProcessor> override
			{ return std::make_unique<T>(Context); }
		};

		template<EMeshPipelinePass Pass> class TBasePassMeshProcessor final : public TMeshPassProcessor<TBasePassMeshProcessor<Pass>>
		{
		public:
			using TMeshPassProcessor<TBasePassMeshProcessor<Pass>>::TMeshPassProcessor;
			auto GetPass() const -> EMeshPipelinePass override { return Pass; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override
			{ return Pass == EMeshPipelinePass::Forward ? Config.Settings.bForward : Config.Settings.bHybrid; }
			auto GetVertexFactoryPass(bool) const -> uint32 override { return MaterialMeshPassForward; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override
			{ return FragmentShader<FSurfaceFragmentShader>(); }
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 8); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey Blend, bool) const -> FMeshPipelineDescription override
			{
				FMeshPipelineDescription Result{.Topology = Topology, .Rasterizer = State.Rasterizer, .Depth = State.Depth};
				if constexpr (Pass == EMeshPipelinePass::Forward) Result.RenderTargets = RenderTargetLayouts::MakeGraphSceneTargets();
				else if constexpr (Pass == EMeshPipelinePass::HybridRetainedForward) Result.RenderTargets = RenderTargetLayouts::MakeGraphHybridRetainedForward();
				else Result.RenderTargets = RenderTargetLayouts::MakeGraphSortedTranslucency();
				Result.ColorBlends[0] = State.ColorBlend;
				if (Blend == EMaterialBlendMode::Translucent) Result.ColorBlends[1].ColorWriteMask = ERHIColorWriteMask::None;
				return Result;
			}
		};
		class FShadowDepthPassMeshProcessor final : public TMeshPassProcessor<FShadowDepthPassMeshProcessor>
		{
		public:
			using TMeshPassProcessor::TMeshPassProcessor;
			auto GetPass() const -> EMeshPipelinePass override { return EMeshPipelinePass::ShadowDepth; }
			auto IsEnabled(const FSceneRenderPreparationConfig&) const -> bool override { return true; }
			auto GetVertexFactoryPass(bool) const -> uint32 override { return MaterialMeshPassShadow; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey Blend) const -> FMeshPassFragmentShader override
			{
				if (Blend == EMaterialBlendMode::Masked) return FragmentShader<FSurfaceMaskedShadowFragmentShader>();
				auto Shader = FragmentShader<FSurfaceOpaqueShadowFragmentShader>();
				Shader.FixedModule = "SurfaceMaterial"; Shader.FixedRequest = "Surface.OpaqueShadow";
				return Shader;
			}
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 2, true); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey, bool) const -> FMeshPipelineDescription override
			{
				const auto Shadow = MakeShadowMeshPipelineState(State);
				return {.RenderTargets = RenderTargetLayouts::MakeDirectionalShadowDepth(), .Topology = Topology,
					.Rasterizer = Shadow.Rasterizer, .Depth = Shadow.Depth};
			}
		};
		class FGBufferPassMeshProcessor final : public TMeshPassProcessor<FGBufferPassMeshProcessor>
		{
		public:
			using TMeshPassProcessor::TMeshPassProcessor;
			auto GetPass() const -> EMeshPipelinePass override { return EMeshPipelinePass::GBuffer; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override { return Config.Settings.bHybrid; }
			auto GetVertexFactoryPass(bool bGPUCulling) const -> uint32 override { return bGPUCulling ? MaterialMeshPassGBufferGPUCulling : MaterialMeshPassGBuffer; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override { return GetGBufferMeshPassFragmentShader(); }
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 8); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey, bool) const -> FMeshPipelineDescription override
			{ return {.RenderTargets = RenderTargetLayouts::MakeGBufferTargets(), .Topology = Topology, .Rasterizer = State.Rasterizer, .Depth = State.Depth}; }
		};
		class FHitProxyPassMeshProcessor final : public TMeshPassProcessor<FHitProxyPassMeshProcessor>
		{
		public:
			using TMeshPassProcessor::TMeshPassProcessor;
			auto GetPass() const -> EMeshPipelinePass override { return EMeshPipelinePass::HitProxy; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override { return Config.Settings.bHitProxy; }
			auto GetVertexFactoryPass(bool) const -> uint32 override { return MaterialMeshPassForward; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override
			{
				auto Shader = GetHitProxyMeshPassFragmentShader();
				Shader.FixedModule = "HitProxy"; Shader.FixedRequest = "Surface.HitProxy";
				return Shader;
			}
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 4, false, true); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey, bool bReversedZ) const -> FMeshPipelineDescription override
			{
				FMeshPipelineDescription Result{.RenderTargets = RenderTargetLayouts::MakeSceneTargets(), .Topology = Topology, .Rasterizer = State.Rasterizer};
				Result.RenderTargets.ColorAttachments[0].RenderTarget.Format = FHitProxyPixel::Format;
				Result.Rasterizer.PolygonMode = ERHIPolygonMode::Fill;
				Result.Depth.bEnableTest = Result.Depth.bEnableWrite = true;
				Result.Depth.CompareOp = bReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::LessOrEqual;
				return Result;
			}
		};
	}

	auto GetMeshPassProcessors() -> std::span<const FMeshPassProcessor* const>
	{
		static const TBasePassMeshProcessor<EMeshPipelinePass::Forward> Forward;
		static const FShadowDepthPassMeshProcessor Shadow;
		static const FGBufferPassMeshProcessor GBuffer;
		static const TBasePassMeshProcessor<EMeshPipelinePass::HybridRetainedForward> Retained;
		static const TBasePassMeshProcessor<EMeshPipelinePass::SortedTranslucency> Translucency;
		static const FHitProxyPassMeshProcessor Picking;
		static const std::array<const FMeshPassProcessor*, 6> Passes{&Forward, &Shadow, &GBuffer, &Retained, &Translucency, &Picking};
		return Passes;
	}
	auto GetMeshPassProcessor(EMeshPipelinePass Pass) -> const FMeshPassProcessor&
	{
		for (const auto* Processor : GetMeshPassProcessors()) if (Processor->GetPass() == Pass) return *Processor;
		requiref(false, "Unregistered mesh pass processor.");
		return *GetMeshPassProcessors().front();
	}
}
