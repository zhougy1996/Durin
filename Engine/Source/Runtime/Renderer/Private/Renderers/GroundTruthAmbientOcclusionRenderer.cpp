#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"

#include "RenderPipelineCreation.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RenderTargetLayouts.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FGTAOVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FGTAOVertexShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Vertex, "VertexMain");
		};

		class FGTAORawFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAORawFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
				DURIN_SHADER_PARAMETER_TEXTURE(RawSelector);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FGTAORawFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "RawFragmentMain");
		};

		class FGTAOSelectorFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAOSelectorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
				DURIN_SHADER_PARAMETER_TEXTURE(RawSelector);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FGTAOSelectorFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "SelectorFragmentMain");
		};

		class FGTAOHalfRawFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAOHalfRawFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
				DURIN_SHADER_PARAMETER_TEXTURE(RawSelector);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FGTAOHalfRawFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "HalfRawFragmentMain");
		};

		class FGTAOFilterFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAOFilterFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(FilterInputVisibility);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterSceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Filter);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterSelector);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FGTAOFilterFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "BilateralFragmentMain");
		};

		#define DURIN_GTAO_HALF_FILTER_PARAMETERS(ShaderType) \
			DURIN_BEGIN_SHADER_PARAMETERS(ShaderType) \
				DURIN_SHADER_PARAMETER_TEXTURE(FilterInputVisibility); \
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferNormals); \
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferSurface); \
				DURIN_SHADER_PARAMETER_TEXTURE(FilterSceneDepth); \
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Filter); \
				DURIN_SHADER_PARAMETER_TEXTURE(FilterSelector); \
			DURIN_END_SHADER_PARAMETERS()

		class FGTAOHalfFilterFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_GTAO_HALF_FILTER_PARAMETERS(FGTAOHalfFilterFragmentShader);
			DURIN_DECLARE_GLOBAL_SHADER(FGTAOHalfFilterFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "HalfBilateralFragmentMain");
		};

		class FGTAOResolveFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_GTAO_HALF_FILTER_PARAMETERS(FGTAOResolveFragmentShader);
			DURIN_DECLARE_GLOBAL_SHADER(FGTAOResolveFragmentShader, FGlobalShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "ResolveFragmentMain");
		};

		#undef DURIN_GTAO_HALF_FILTER_PARAMETERS
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAORawFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOSelectorFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOHalfRawFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOFilterFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOHalfFilterFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGTAOResolveFragmentShader);
		const FGlobalShaderSetRegistration GGtaoRawShaderSet(
			"Renderer", "GroundTruthAmbientOcclusion.Raw",
			EShaderRequestEligibility::GameAndEditor,
			{&FGTAOVertexShader::StaticType(),
			 &FGTAORawFragmentShader::StaticType(),
			 &FGTAOSelectorFragmentShader::StaticType(),
			 &FGTAOHalfRawFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GGtaoFilterShaderSet(
			"Renderer", "GroundTruthAmbientOcclusion.Filter",
			EShaderRequestEligibility::GameAndEditor,
			{&FGTAOVertexShader::StaticType(),
			 &FGTAOFilterFragmentShader::StaticType(),
			 &FGTAOHalfFilterFragmentShader::StaticType(),
			 &FGTAOResolveFragmentShader::StaticType()});
	}

	namespace
	{
		template<typename TPixelShader>
		auto MakeGTAOPipeline(const TShaderMapRef<FGTAOVertexShader>& Vertex, const TShaderMapRef<TPixelShader>& Pixel,
			const FVertexDeclarationRHIRef& Declaration, const FRasterizerStateRHIRef& Rasterizer,
			FGraphicsPipelineStateInitializer& Pipeline) -> bool
		{
			Pipeline.BoundShaders.VertexShader = Vertex.GetRHIShader(false);
			Pipeline.BoundShaders.FragmentShader = Pixel.GetRHIShader(false);
			if (!Pipeline.BoundShaders.VertexShader || !Pipeline.BoundShaders.FragmentShader) return false;
			Pipeline.VertexDeclaration = Declaration;
			Pipeline.RasterizerState = Rasterizer;
			Pipeline.RenderTargetLayout = RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
			Pipeline.PipelineLayout = Pixel.GetPipelineLayout();
			return true;
		}
	}

	FGroundTruthAmbientOcclusionRenderer::FGroundTruthAmbientOcclusionRenderer(
		FFullscreenGeometryResources& InFullscreenGeometry)
		: FullscreenGeometry(InFullscreenGeometry)
	{
	}

	FGroundTruthAmbientOcclusionRenderer::~FGroundTruthAmbientOcclusionRenderer() = default;

	auto FGroundTruthAmbientOcclusionRenderer::EnsureGeometry_RenderThread(FRHICommandListImmediate& CommandList) -> bool
	{
		if (!Rasterizer) Rasterizer = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
		return Rasterizer && FullscreenGeometry.EnsureResources_RenderThread(CommandList)
			&& FullscreenGeometry.GetVertexBuffer_RenderThread() && FullscreenGeometry.GetIndexBuffer_RenderThread();
	}

	auto FGroundTruthAmbientOcclusionRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList, std::optional<EGroundTruthAmbientOcclusionQuality> Quality) -> bool
	{
		if (!EnsureGeometry_RenderThread(CommandList)) return false;
		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FGTAOVertexShader>();
		if (!Vertex.GetRHIShader(false)) return false;
		const auto Check = [&](const auto& Pixel, const char* Name) {
			if (!FRenderPipelinePreparationBatch::ShouldPrecachePipelines()) return Pixel.GetRHIShader(false) != nullptr;
			FGraphicsPipelineStateInitializer Pipeline;
			if (!MakeGTAOPipeline(Vertex, Pixel, FullscreenGeometry.GetVertexDeclaration_RenderThread(), Rasterizer, Pipeline))
				return false;
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Pipeline, Name);
			if (!Request) return false;
			FRenderPipelinePreparationBatch::Add(Request);
			const auto Status = Request->GetState();
			return Status != ERHIPipelineState::Failed && Status != ERHIPipelineState::Canceled;
		};
		if (Quality != EGroundTruthAmbientOcclusionQuality::HalfResolution
			&& (!Check(Map.GetShader<FGTAORawFragmentShader>(), "GroundTruthAmbientOcclusionRawPipeline")
				|| !Check(Map.GetShader<FGTAOFilterFragmentShader>(), "GroundTruthAmbientOcclusionFilterPipeline"))) return false;
		return Quality == EGroundTruthAmbientOcclusionQuality::FullResolution
			|| (Check(Map.GetShader<FGTAOSelectorFragmentShader>(), "GroundTruthAmbientOcclusionSelectorPipeline")
				&& Check(Map.GetShader<FGTAOHalfRawFragmentShader>(), "GroundTruthAmbientOcclusionHalfRawPipeline")
				&& Check(Map.GetShader<FGTAOHalfFilterFragmentShader>(), "GroundTruthAmbientOcclusionHalfFilterPipeline")
				&& Check(Map.GetShader<FGTAOResolveFragmentShader>(), "GroundTruthAmbientOcclusionResolvePipeline"));
	}

	auto FGroundTruthAmbientOcclusionRenderer::DescribeTargets(
		uint32 Width, uint32 Height,
		EGroundTruthAmbientOcclusionQuality Quality)
		-> std::vector<FRHITextureCreateDesc>
	{
		const bool bHalf =
			Quality == EGroundTruthAmbientOcclusionQuality::HalfResolution;
		const uint32 NativeWidth = bHalf ? CalculateHalfExtent(Width) : Width;
		const uint32 NativeHeight = bHalf ? CalculateHalfExtent(Height) : Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			bHalf ? "GroundTruthAmbientOcclusionHalfRaw"
				: "GroundTruthAmbientOcclusionRaw",
			NativeWidth, NativeHeight,
			EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		auto ScratchDesc = Desc;
		ScratchDesc.DebugName = bHalf
			? "GroundTruthAmbientOcclusionHalfScratch"
			: "GroundTruthAmbientOcclusionScratch";
		std::vector<FRHITextureCreateDesc> Descriptions{Desc, ScratchDesc};
		if (bHalf)
		{
			auto SelectorDesc = Desc;
			SelectorDesc.DebugName =
				"GroundTruthAmbientOcclusionRepresentativeSelector";
			SelectorDesc.ClearValue =
				FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
			auto ResolvedDesc = FRHITextureCreateDesc::Create2D(
				"GroundTruthAmbientOcclusionResolved", Width, Height,
				EPixelFormat::R8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy)
				.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
			Descriptions.push_back(SelectorDesc);
			Descriptions.push_back(ResolvedDesc);
		}
		return Descriptions;
	}

	auto FGroundTruthAmbientOcclusionRenderer::RenderRaw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets& Targets,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Depth,
		const FSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Raw == nullptr || Normals == nullptr || Surface == nullptr
			|| Depth == nullptr || View.ViewportWidth == 0
			|| View.ViewportHeight == 0
			|| (Targets.Quality
					== EGroundTruthAmbientOcclusionQuality::HalfResolution
				&& Targets.Selector == nullptr))
			return false;
		if (!EnsureGeometry_RenderThread(CommandList)) return false;
		const bool bHalf = Targets.Quality == EGroundTruthAmbientOcclusionQuality::HalfResolution;
		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FGTAOVertexShader>();
		const auto& Declaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		TShaderMapRef<FGTAORawFragmentShader> FullShader;
		TShaderMapRef<FGTAOHalfRawFragmentShader> HalfShader;
		FGraphicsPipelineStateInitializer Pipeline;
		TShaderMapRef<FGTAOSelectorFragmentShader> SelectorShader;
		FGraphicsPipelineStateInitializer SelectorPipeline;
		if (bHalf)
		{
			HalfShader = Map.GetShader<FGTAOHalfRawFragmentShader>();
			if (!MakeGTAOPipeline(Vertex, HalfShader, Declaration, Rasterizer, Pipeline)) return false;
			SelectorShader = Map.GetShader<FGTAOSelectorFragmentShader>();
			if (!MakeGTAOPipeline(Vertex, SelectorShader, Declaration, Rasterizer, SelectorPipeline)) return false;
		}
		else
		{
			FullShader = Map.GetShader<FGTAORawFragmentShader>();
			if (!MakeGTAOPipeline(Vertex, FullShader, Declaration, Rasterizer, Pipeline)) return false;
		}

		FViewUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
				Uniform.WorldToViewRows[Row][Col] =
					static_cast<float>(View.GetTranslatedWorldToView()[Col][Row]);
			}
		}
		Uniform.Viewport = {
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight)};
		const FRectangle HalfViewport = MapFullRectangleToHalf({
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight});
		Uniform.HalfViewport = {
			static_cast<float>(HalfViewport.X),
			static_cast<float>(HalfViewport.Y),
			static_cast<float>(HalfViewport.Width),
			static_cast<float>(HalfViewport.Height)};
		Uniform.Controls[2] = bHalf ? 48.0f : 96.0f;
		const FRHIUniformBufferRange ViewUniform =
			CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
		if (ViewUniform.Buffer == nullptr || ViewUniform.Size != sizeof(Uniform))
			return false;
		const FRectangle RenderViewport = bHalf ? HalfViewport : FRectangle{
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight};
		if (bHalf)
		{
			FRHIRenderPassInfo SelectorPassInfo{};
			SelectorPassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
			SelectorPassInfo.ColorRenderTargets[0] = Targets.Selector;
			SelectorPassInfo.ColorClearValues[0] =
				FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
			CommandList.BeginRenderPass(
				SelectorPassInfo, "GroundTruthAmbientOcclusionSelectorPass");
			SetGraphicsPipelineState(CommandList, SelectorPipeline);
			CommandList.SetViewport(
				static_cast<float>(RenderViewport.X),
				static_cast<float>(RenderViewport.Y), 0.0f,
				static_cast<float>(RenderViewport.X + RenderViewport.Width),
				static_cast<float>(RenderViewport.Y + RenderViewport.Height), 1.0f);
			CommandList.SetScissor(
				static_cast<float>(RenderViewport.X),
				static_cast<float>(RenderViewport.Y),
				static_cast<float>(RenderViewport.Width),
				static_cast<float>(RenderViewport.Height));
			CommandList.BindVertexBuffer(0,
				FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
			CommandList.BindIndexBuffer(
				FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
			FGTAOSelectorFragmentShader::FParameters SelectorParameters;
			SelectorParameters.GBufferNormals = Normals;
			SelectorParameters.GBufferSurface = Surface;
			SelectorParameters.SceneDepth = Depth;
			SelectorParameters.View = ViewUniform;
			SelectorParameters.RawSelector = Surface;
			SetShaderParameters(CommandList,
				SelectorShader, SelectorParameters);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
		}

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
		PassInfo.ColorRenderTargets[0] = Targets.Raw;
		PassInfo.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "GroundTruthAmbientOcclusionRawPass");
		SetGraphicsPipelineState(CommandList, Pipeline);
		CommandList.SetViewport(
			static_cast<float>(RenderViewport.X),
			static_cast<float>(RenderViewport.Y), 0.0f,
			static_cast<float>(RenderViewport.X + RenderViewport.Width),
			static_cast<float>(RenderViewport.Y + RenderViewport.Height), 1.0f);
		CommandList.SetScissor(
			static_cast<float>(RenderViewport.X),
			static_cast<float>(RenderViewport.Y),
			static_cast<float>(RenderViewport.Width),
			static_cast<float>(RenderViewport.Height));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		if (bHalf)
		{
			FGTAOHalfRawFragmentShader::FParameters Parameters;
			Parameters.GBufferNormals = Normals;
			Parameters.GBufferSurface = Surface;
			Parameters.SceneDepth = Depth;
			Parameters.View = ViewUniform;
			Parameters.RawSelector = Targets.Selector;
			SetShaderParameters(
				CommandList, HalfShader, Parameters);
		}
		else
		{
			FGTAORawFragmentShader::FParameters Parameters;
			Parameters.GBufferNormals = Normals;
			Parameters.GBufferSurface = Surface;
			Parameters.SceneDepth = Depth;
			Parameters.View = ViewUniform;
			Parameters.RawSelector = Surface;
			SetShaderParameters(CommandList, FullShader, Parameters);
		}
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FGroundTruthAmbientOcclusionRenderer::RenderFilter_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets& Targets,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Depth,
		const FSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Raw == nullptr || Targets.Scratch == nullptr
			|| Normals == nullptr || Surface == nullptr || Depth == nullptr
			|| View.ViewportWidth == 0 || View.ViewportHeight == 0
			|| (Targets.Quality
					== EGroundTruthAmbientOcclusionQuality::HalfResolution
				&& Targets.Selector == nullptr))
			return false;
		if (!EnsureGeometry_RenderThread(CommandList)) return false;
		const bool bHalf = Targets.Quality == EGroundTruthAmbientOcclusionQuality::HalfResolution;
		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FGTAOVertexShader>();
		const auto& Declaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		TShaderMapRef<FGTAOFilterFragmentShader> FullShader;
		TShaderMapRef<FGTAOHalfFilterFragmentShader> HalfShader;
		FGraphicsPipelineStateInitializer Pipeline;
		if (bHalf)
		{
			HalfShader = Map.GetShader<FGTAOHalfFilterFragmentShader>();
			if (!MakeGTAOPipeline(Vertex, HalfShader, Declaration, Rasterizer, Pipeline)) return false;
		}
		else
		{
			FullShader = Map.GetShader<FGTAOFilterFragmentShader>();
			if (!MakeGTAOPipeline(Vertex, FullShader, Declaration, Rasterizer, Pipeline)) return false;
		}

		FFilterUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
			}
		}
		Uniform.Viewport = {
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight)};
		const FRectangle HalfViewport = MapFullRectangleToHalf({
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight});
		Uniform.HalfViewport = {
			static_cast<float>(HalfViewport.X),
			static_cast<float>(HalfViewport.Y),
			static_cast<float>(HalfViewport.Width),
			static_cast<float>(HalfViewport.Height)};
		const FRectangle RenderViewport = bHalf ? HalfViewport : FRectangle{
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight};

		auto RenderDirection = [&](FRHITexture* Input, FRHITexture* Output,
			float DirectionX, float DirectionY, const char* PassName) {
			Uniform.DirectionAndThresholds = {
				DirectionX, DirectionY, 0.90f, 0.01f};
			const FRHIUniformBufferRange FilterUniform =
				CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
			if (FilterUniform.Buffer == nullptr
				|| FilterUniform.Size != sizeof(Uniform))
				return false;
			FRHIRenderPassInfo PassInfo{};
			PassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
			PassInfo.ColorRenderTargets[0] = Output;
			PassInfo.ColorClearValues[0] =
				FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
			CommandList.BeginRenderPass(PassInfo, PassName);
			SetGraphicsPipelineState(CommandList, Pipeline);
			CommandList.SetViewport(
				static_cast<float>(RenderViewport.X),
				static_cast<float>(RenderViewport.Y), 0.0f,
				static_cast<float>(RenderViewport.X + RenderViewport.Width),
				static_cast<float>(RenderViewport.Y + RenderViewport.Height), 1.0f);
			CommandList.SetScissor(
				static_cast<float>(RenderViewport.X),
				static_cast<float>(RenderViewport.Y),
				static_cast<float>(RenderViewport.Width),
				static_cast<float>(RenderViewport.Height));
			CommandList.BindVertexBuffer(0,
				FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
			CommandList.BindIndexBuffer(
				FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
			if (bHalf)
			{
				FGTAOHalfFilterFragmentShader::FParameters Parameters;
				Parameters.FilterInputVisibility = Input;
				Parameters.FilterGBufferNormals = Normals;
				Parameters.FilterGBufferSurface = Surface;
				Parameters.FilterSceneDepth = Depth;
				Parameters.Filter = FilterUniform;
				Parameters.FilterSelector = Targets.Selector;
				SetShaderParameters(CommandList,
					HalfShader, Parameters);
			}
			else
			{
				FGTAOFilterFragmentShader::FParameters Parameters;
				Parameters.FilterInputVisibility = Input;
				Parameters.FilterGBufferNormals = Normals;
				Parameters.FilterGBufferSurface = Surface;
				Parameters.FilterSceneDepth = Depth;
				Parameters.Filter = FilterUniform;
				Parameters.FilterSelector = Input;
				SetShaderParameters(
					CommandList, FullShader, Parameters);
			}
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
			return true;
		};

		return RenderDirection(
			Targets.Raw, Targets.Scratch, 1.0f, 0.0f,
			"GroundTruthAmbientOcclusionHorizontalFilter")
			&& RenderDirection(
				Targets.Scratch, Targets.Raw, 0.0f, 1.0f,
				"GroundTruthAmbientOcclusionVerticalFilter");
	}

	auto FGroundTruthAmbientOcclusionRenderer::RenderResolve_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets& Targets,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Depth,
		const FSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Quality
			!= EGroundTruthAmbientOcclusionQuality::HalfResolution)
			return true;
		if (Targets.Raw == nullptr || Targets.Selector == nullptr
			|| Targets.Resolved == nullptr || Normals == nullptr
			|| Surface == nullptr || Depth == nullptr
			|| View.ViewportWidth == 0 || View.ViewportHeight == 0)
			return false;
		if (!EnsureGeometry_RenderThread(CommandList)) return false;
		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FGTAOVertexShader>();
		const auto ResolveShader = Map.GetShader<FGTAOResolveFragmentShader>();
		FGraphicsPipelineStateInitializer Pipeline;
		if (!MakeGTAOPipeline(Vertex, ResolveShader, FullscreenGeometry.GetVertexDeclaration_RenderThread(), Rasterizer, Pipeline))
			return false;

		FFilterUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
			}
		}
		Uniform.Viewport = {
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight)};
		const FRectangle HalfViewport = MapFullRectangleToHalf({
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight});
		Uniform.HalfViewport = {
			static_cast<float>(HalfViewport.X),
			static_cast<float>(HalfViewport.Y),
			static_cast<float>(HalfViewport.Width),
			static_cast<float>(HalfViewport.Height)};
		Uniform.DirectionAndThresholds = {0.0f, 0.0f, 0.90f, 0.01f};
		const FRHIUniformBufferRange ResolveUniform =
			CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
		if (ResolveUniform.Buffer == nullptr
			|| ResolveUniform.Size != sizeof(Uniform))
			return false;

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
		PassInfo.ColorRenderTargets[0] = Targets.Resolved;
		PassInfo.ColorClearValues[0] =
			FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(
			PassInfo, "GroundTruthAmbientOcclusionResolvePass");
		SetGraphicsPipelineState(CommandList, Pipeline);
		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth),
			static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FGTAOResolveFragmentShader::FParameters Parameters;
		Parameters.FilterInputVisibility = Targets.Raw;
		Parameters.FilterGBufferNormals = Normals;
		Parameters.FilterGBufferSurface = Surface;
		Parameters.FilterSceneDepth = Depth;
		Parameters.Filter = ResolveUniform;
		Parameters.FilterSelector = Targets.Selector;
		SetShaderParameters(
			CommandList, ResolveShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FGroundTruthAmbientOcclusionRenderer::ReleaseResources_RenderThread()
		-> void
	{
		Rasterizer.reset();
	}
}
