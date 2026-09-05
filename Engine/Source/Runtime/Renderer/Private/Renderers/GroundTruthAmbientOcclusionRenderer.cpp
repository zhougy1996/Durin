#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
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

	struct FGroundTruthAmbientOcclusionRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef RawShaderSet;
			FGlobalShaderSetRef FilterShaderSet;
			TShaderMapRef<FGTAOVertexShader> RawVertexShader;
			TShaderMapRef<FGTAOVertexShader> FilterVertexShader;
			TShaderMapRef<FGTAORawFragmentShader> FragmentShader;
			TShaderMapRef<FGTAOSelectorFragmentShader> SelectorFragmentShader;
			TShaderMapRef<FGTAOHalfRawFragmentShader> HalfFragmentShader;
			TShaderMapRef<FGTAOFilterFragmentShader> FilterFragmentShader;
			TShaderMapRef<FGTAOHalfFilterFragmentShader> HalfFilterFragmentShader;
			TShaderMapRef<FGTAOResolveFragmentShader> ResolveFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
			FGraphicsPipelineStateRHIRef SelectorPipelineState;
			FGraphicsPipelineStateRHIRef HalfPipelineState;
			FGraphicsPipelineStateRHIRef FilterPipelineState;
			FGraphicsPipelineStateRHIRef HalfFilterPipelineState;
			FGraphicsPipelineStateRHIRef ResolvePipelineState;
		};

		TRenderResourceCreationSlot<FPayload> Resources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FGroundTruthAmbientOcclusionRenderer::FGroundTruthAmbientOcclusionRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FGroundTruthAmbientOcclusionRenderer::~FGroundTruthAmbientOcclusionRenderer() = default;

	auto FGroundTruthAmbientOcclusionRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				const std::array<const FGlobalShaderType*, 4> RawTypes{
					&FGTAOVertexShader::StaticType(),
					&FGTAORawFragmentShader::StaticType(),
					&FGTAOSelectorFragmentShader::StaticType(),
					&FGTAOHalfRawFragmentShader::StaticType()};
				const std::array<const FGlobalShaderType*, 4> FilterTypes{
					&FGTAOVertexShader::StaticType(),
					&FGTAOFilterFragmentShader::StaticType(),
					&FGTAOHalfFilterFragmentShader::StaticType(),
					&FGTAOResolveFragmentShader::StaticType()};
				FPayload Candidate;
				Candidate.RawShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"GroundTruthAmbientOcclusion.Raw", RawTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.RawShaderSet)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GroundTruthAmbientOcclusion", "raw-shader",
						"Global shader set is unavailable.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual,
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				}
				Candidate.FilterShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"GroundTruthAmbientOcclusion.Filter", FilterTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.FilterShaderSet)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GroundTruthAmbientOcclusion", "filter-shader",
						"Global shader set is unavailable.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual,
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				}
				Candidate.RawVertexShader = TShaderMapRef<FGTAOVertexShader>(Candidate.RawShaderSet);
				Candidate.FilterVertexShader = TShaderMapRef<FGTAOVertexShader>(Candidate.FilterShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FGTAORawFragmentShader>(Candidate.RawShaderSet);
				Candidate.SelectorFragmentShader = TShaderMapRef<FGTAOSelectorFragmentShader>(Candidate.RawShaderSet);
				Candidate.HalfFragmentShader = TShaderMapRef<FGTAOHalfRawFragmentShader>(Candidate.RawShaderSet);
				Candidate.FilterFragmentShader = TShaderMapRef<FGTAOFilterFragmentShader>(Candidate.FilterShaderSet);
				Candidate.HalfFilterFragmentShader = TShaderMapRef<FGTAOHalfFilterFragmentShader>(Candidate.FilterShaderSet);
				Candidate.ResolveFragmentShader = TShaderMapRef<FGTAOResolveFragmentShader>(Candidate.FilterShaderSet);
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GroundTruthAmbientOcclusion", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				FRHIShader* RawVertexRHI =
					Candidate.RawVertexShader.GetRHIShader(false);
				FRHIShader* FilterVertexRHI =
					Candidate.FilterVertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				FRHIShader* SelectorRHI =
					Candidate.SelectorFragmentShader.GetRHIShader(false);
				FRHIShader* HalfFragmentRHI =
					Candidate.HalfFragmentShader.GetRHIShader(false);
				FRHIShader* FilterRHI =
					Candidate.FilterFragmentShader.GetRHIShader(false);
				FRHIShader* HalfFilterRHI =
					Candidate.HalfFilterFragmentShader.GetRHIShader(false);
				FRHIShader* ResolveRHI =
					Candidate.ResolveFragmentShader.GetRHIShader(false);
				if (RawVertexRHI == nullptr || FilterVertexRHI == nullptr
					|| FragmentRHI == nullptr || SelectorRHI == nullptr
					|| HalfFragmentRHI == nullptr
					|| FilterRHI == nullptr || HalfFilterRHI == nullptr
					|| ResolveRHI == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GroundTruthAmbientOcclusion", "raw-shader",
						"RHI shader creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
				Initializer.BoundShaders.VertexShader = RawVertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.RawShaderSet.GetPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionRawPipeline", Initializer);
				if (Candidate.PipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "raw-pipeline",
						"RHI graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
				Initializer.BoundShaders.FragmentShader = SelectorRHI;
				Candidate.SelectorPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionSelectorPipeline", Initializer);
				Initializer.BoundShaders.FragmentShader = HalfFragmentRHI;
				Candidate.HalfPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionHalfRawPipeline", Initializer);
				if (Candidate.SelectorPipelineState == nullptr
					|| Candidate.HalfPipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "half-raw-pipeline",
						"RHI graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
				Initializer.BoundShaders.VertexShader = FilterVertexRHI;
				Initializer.BoundShaders.FragmentShader = FilterRHI;
				Initializer.PipelineLayout =
					Candidate.FilterShaderSet.GetPipelineLayout();
				Candidate.FilterPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionFilterPipeline", Initializer);
				if (Candidate.FilterPipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "filter-pipeline",
						"RHI graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				Initializer.BoundShaders.FragmentShader = HalfFilterRHI;
				Candidate.HalfFilterPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionHalfFilterPipeline", Initializer);
				Initializer.BoundShaders.FragmentShader = ResolveRHI;
				Candidate.ResolvePipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionResolvePipeline", Initializer);
				if (Candidate.HalfFilterPipelineState == nullptr
					|| Candidate.ResolvePipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "half-filter-resolve-pipeline",
						"One or more RHI graphics pipelines returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		return Payload != nullptr;
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
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		const bool bHalf = Targets.Quality
			== EGroundTruthAmbientOcclusionQuality::HalfResolution;
		FGraphicsPipelineStateRHIRef Pipeline = bHalf
			? Payload != nullptr ? Payload->HalfPipelineState : nullptr
			: Payload != nullptr ? Payload->PipelineState : nullptr;
		if (Payload == nullptr || Pipeline == nullptr
			|| (bHalf && Payload->SelectorPipelineState == nullptr)
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
			return false;

		FViewUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
				Uniform.WorldToViewRows[Row][Col] =
					static_cast<float>(View.ViewMatrix[Col][Row]);
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
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
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
			CommandList.SetGraphicsPipelineState(*Payload->SelectorPipelineState);
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
				Payload->SelectorFragmentShader, SelectorParameters);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
		}

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
		PassInfo.ColorRenderTargets[0] = Targets.Raw;
		PassInfo.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "GroundTruthAmbientOcclusionRawPass");
		CommandList.SetGraphicsPipelineState(*Pipeline);
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
				CommandList, Payload->HalfFragmentShader, Parameters);
		}
		else
		{
			FGTAORawFragmentShader::FParameters Parameters;
			Parameters.GBufferNormals = Normals;
			Parameters.GBufferSurface = Surface;
			Parameters.SceneDepth = Depth;
			Parameters.View = ViewUniform;
			Parameters.RawSelector = Surface;
			SetShaderParameters(CommandList, Payload->FragmentShader, Parameters);
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
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		const bool bHalf = Targets.Quality
			== EGroundTruthAmbientOcclusionQuality::HalfResolution;
		FGraphicsPipelineStateRHIRef Pipeline = bHalf
			? Payload != nullptr ? Payload->HalfFilterPipelineState : nullptr
			: Payload != nullptr ? Payload->FilterPipelineState : nullptr;
		if (Payload == nullptr || Pipeline == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
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
		const FRectangle RenderViewport = bHalf ? HalfViewport : FRectangle{
			View.ViewportX, View.ViewportY,
			View.ViewportWidth, View.ViewportHeight};

		auto RenderDirection = [&](FRHITexture* Input, FRHITexture* Output,
			float DirectionX, float DirectionY, const char* PassName) {
			Uniform.DirectionAndThresholds = {
				DirectionX, DirectionY, 0.90f, 0.01f};
			const FRHIUniformBufferRange FilterUniform =
				CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
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
			CommandList.SetGraphicsPipelineState(*Pipeline);
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
					Payload->HalfFilterFragmentShader, Parameters);
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
					CommandList, Payload->FilterFragmentShader, Parameters);
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
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		if (Payload == nullptr || Payload->ResolvePipelineState == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
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
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
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
		CommandList.SetGraphicsPipelineState(*Payload->ResolvePipelineState);
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
			CommandList, Payload->ResolveFragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FGroundTruthAmbientOcclusionRenderer::ReleaseResources_RenderThread()
		-> void
	{
		State->Resources.Reset();
	}
}
