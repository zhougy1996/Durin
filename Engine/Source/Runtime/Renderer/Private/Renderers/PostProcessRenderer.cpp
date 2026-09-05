#include "Renderers/PostProcessRenderer.h"

#include "Renderers/DisplayMapping.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FPostProcessVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(
				FPostProcessVertexShader,
				FGlobalShader,
				"/Engine/PostProcess",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FCopySceneColorFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCopySceneColorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FCopySceneColorFragmentShader,
				FGlobalShader,
				"/Engine/PostProcess",
				EShaderFrequency::Fragment,
				"CopyFragmentMain");
		};

		class FFXAAFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FFXAAFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FFXAAFragmentShader,
				FGlobalShader,
				"/Engine/PostProcess",
				EShaderFrequency::Fragment,
				"FXAAFragmentMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FPostProcessVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FCopySceneColorFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FFXAAFragmentShader);
		const FGlobalShaderSetRegistration GPostProcessCopyShaderSet(
			"Renderer", "PostProcess.Copy",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(),
			 &FCopySceneColorFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GPostProcessFxaaShaderSet(
			"Renderer", "PostProcess.FXAA",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(),
			 &FFXAAFragmentShader::StaticType()});

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			float ExposureScale = 1.0f;
			float Padding = 0.0f;
		};
		static_assert(sizeof(FPostProcessViewUniform) == 16);
		static_assert(alignof(FPostProcessViewUniform) == alignof(float));

		auto CreatePostProcessPipeline(
			FName PipelineName,
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FVertexDeclarationRHIRef& VertexDeclaration,
			const FPipelineLayoutDesc& PipelineLayout,
			const FRHIRenderTargetLayout& RenderTargetLayout)
			-> FGraphicsPipelineStateRHIRef
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			Initializer.RasterizerState.CullMode = ERHICullMode::None;
			Initializer.PipelineLayout = PipelineLayout;
			return GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName,
				Initializer);
		}
	} // namespace

	struct FPostProcessRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef CopyShaderSet;
			FGlobalShaderSetRef FXAAShaderSet;
			TShaderMapRef<FPostProcessVertexShader> CopyVertexShader;
			TShaderMapRef<FPostProcessVertexShader> FXAAVertexShader;
			TShaderMapRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderMapRef<FFXAAFragmentShader> FXAAFragmentShader;
			FGraphicsPipelineStateRHIRef CopyIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef FXAAIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef CopyBGRAIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef FXAABGRAIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef CopyOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef CopyBGRAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef CopyPresentPipelineState;
			FGraphicsPipelineStateRHIRef CopyBGRAPresentPipelineState;
			FGraphicsPipelineStateRHIRef FXAAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef FXAABGRAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef FXAAPresentPipelineState;
			FGraphicsPipelineStateRHIRef FXAABGRAPresentPipelineState;
			FSamplerRHIRef SceneColorSampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FPostProcessRenderer::FPostProcessRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FPostProcessRenderer::~FPostProcessRenderer() = default;

	auto FPostProcessRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;

		FPayload* Payload = State->Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> CopyShaderTypes = {
					&FPostProcessVertexShader::StaticType(),
					&FCopySceneColorFragmentShader::StaticType()};
				const std::array<const FGlobalShaderType*, 2> FXAAShaderTypes = {
					&FPostProcessVertexShader::StaticType(),
					&FFXAAFragmentShader::StaticType()};
				FPayload Candidate;
				Candidate.CopyShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"PostProcess.Copy", CopyShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.CopyShaderSet)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"copy",
							"Global shader set is unavailable.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual,
							ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				}
				Candidate.FXAAShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"PostProcess.FXAA", FXAAShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.FXAAShaderSet)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"fxaa",
							"Global shader set is unavailable.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual,
							ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				}

				Candidate.CopyVertexShader = TShaderMapRef<FPostProcessVertexShader>(Candidate.CopyShaderSet);
				Candidate.FXAAVertexShader = TShaderMapRef<FPostProcessVertexShader>(Candidate.FXAAShaderSet);
				Candidate.CopyFragmentShader = TShaderMapRef<FCopySceneColorFragmentShader>(Candidate.CopyShaderSet);
				Candidate.FXAAFragmentShader = TShaderMapRef<FFXAAFragmentShader>(Candidate.FXAAShaderSet);

				if (!FullscreenGeometry.EnsureResources_RenderThread(
						CommandList))
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"PostProcess",
							"fullscreen-geometry",
							"Shared fullscreen geometry is unavailable.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}

				Candidate.SceneColorSampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				FRHIShader* CopyVertexRHI =
					Candidate.CopyVertexShader.GetRHIShader(false);
				FRHIShader* CopyFragmentRHI =
					Candidate.CopyFragmentShader.GetRHIShader(false);
				FRHIShader* FXAAVertexRHI =
					Candidate.FXAAVertexShader.GetRHIShader(false);
				FRHIShader* FXAAFragmentRHI =
					Candidate.FXAAFragmentShader.GetRHIShader(false);
				if (Candidate.SceneColorSampler == nullptr
					|| CopyVertexRHI == nullptr || CopyFragmentRHI == nullptr
					|| FXAAVertexRHI == nullptr || FXAAFragmentRHI == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"PostProcess",
							"copy+fxaa",
							"RHI shader or sampler creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				const FVertexDeclarationRHIRef& VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				auto MakePipeline =
					[&Candidate, &VertexDeclaration](
						FName Name,
						FRHIShader* VertexShader,
						FRHIShader* FragmentShader,
						const FPipelineLayoutDesc& Layout,
						const FRHIRenderTargetLayout& RenderTargetLayout) {
						return CreatePostProcessPipeline(
							Name,
							VertexShader,
							FragmentShader,
							VertexDeclaration,
							Layout,
							RenderTargetLayout);
					};
				Candidate.CopyIntermediatePipelineState = MakePipeline(
					"PostProcessCopyIntermediatePipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput());
				Candidate.FXAAIntermediatePipelineState = MakePipeline(
					"PostProcessFXAAIntermediatePipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput());
				Candidate.CopyBGRAIntermediatePipelineState = MakePipeline(
					"PostProcessCopyBGRAIntermediatePipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput(
						EPixelFormat::SBGRA8_UNORM));
				Candidate.FXAABGRAIntermediatePipelineState = MakePipeline(
					"PostProcessFXAABGRAIntermediatePipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput(
						EPixelFormat::SBGRA8_UNORM));
				Candidate.CopyOffscreenPipelineState = MakePipeline(
					"PostProcessCopyOffscreenPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen));
				Candidate.CopyPresentPipelineState = MakePipeline(
					"PostProcessCopyPresentPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present));
				Candidate.CopyBGRAOffscreenPipelineState = MakePipeline(
					"PostProcessCopyBGRAOffscreenPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen,
						EPixelFormat::SBGRA8_UNORM));
				Candidate.FXAAOffscreenPipelineState = MakePipeline(
					"PostProcessFXAAOffscreenPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen));
				Candidate.FXAAPresentPipelineState = MakePipeline(
					"PostProcessFXAAPresentPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present));
				Candidate.FXAABGRAOffscreenPipelineState = MakePipeline(
					"PostProcessFXAABGRAOffscreenPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen,
						EPixelFormat::SBGRA8_UNORM));
				Candidate.CopyBGRAPresentPipelineState = MakePipeline(
					"PostProcessCopyBGRAPresentPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present,
						EPixelFormat::SBGRA8_UNORM));
				Candidate.FXAABGRAPresentPipelineState = MakePipeline(
					"PostProcessFXAABGRAPresentPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderSet.GetPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present,
						EPixelFormat::SBGRA8_UNORM));
				if (Candidate.CopyIntermediatePipelineState == nullptr
					|| Candidate.FXAAIntermediatePipelineState == nullptr
					|| Candidate.CopyBGRAIntermediatePipelineState == nullptr
					|| Candidate.FXAABGRAIntermediatePipelineState == nullptr
					|| Candidate.CopyOffscreenPipelineState == nullptr
					|| Candidate.CopyBGRAOffscreenPipelineState == nullptr
					|| Candidate.CopyPresentPipelineState == nullptr
					|| Candidate.CopyBGRAPresentPipelineState == nullptr
					|| Candidate.FXAAOffscreenPipelineState == nullptr
					|| Candidate.FXAABGRAOffscreenPipelineState == nullptr
					|| Candidate.FXAAPresentPipelineState == nullptr
					|| Candidate.FXAABGRAPresentPipelineState == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"PostProcess",
							"copy+fxaa",
							"RHI resource or pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		return Payload != nullptr;
	}

	auto FPostProcessRenderer::DescribeSceneTargets(uint32 Width, uint32 Height)
		-> std::array<FRHITextureCreateDesc, 2>
	{
		return {
			FRHITextureCreateDesc::Create2D("SceneColor", Width, Height,
				EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy)
				.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
			FRHITextureCreateDesc::Create2D("SceneDepth", Width, Height,
				EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource)
				.SetClearValue(FClearValueBinding(0.0f, 0u))};
	}
	auto FPostProcessRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		uint32 Width,
		uint32 Height,
		bool bPresentOutput,
		bool bEnableFXAA,
		bool bHasEditorAssistance,
		EPixelFormat OutputFormat,
		float ExposureEV) -> void
	{
		const FState::FPayload* Payload = State->Slot.GetPayload();
		if (Payload == nullptr)
		{
			return;
		}

		const bool bBGRAOutput = OutputFormat == EPixelFormat::SBGRA8_UNORM;
		FGraphicsPipelineStateRHIRef PipelineState;
		if (bHasEditorAssistance)
		{
			PipelineState = bEnableFXAA
				? (bBGRAOutput ? Payload->FXAABGRAIntermediatePipelineState
					: Payload->FXAAIntermediatePipelineState)
				: (bBGRAOutput ? Payload->CopyBGRAIntermediatePipelineState
					: Payload->CopyIntermediatePipelineState);
		}
		else
		{
			PipelineState = bEnableFXAA
				? (bPresentOutput
					? (bBGRAOutput ? Payload->FXAABGRAPresentPipelineState
						: Payload->FXAAPresentPipelineState)
					: (bBGRAOutput ? Payload->FXAABGRAOffscreenPipelineState
						: Payload->FXAAOffscreenPipelineState))
				: (bPresentOutput
					? (bBGRAOutput ? Payload->CopyBGRAPresentPipelineState
						: Payload->CopyPresentPipelineState)
					: (bBGRAOutput ? Payload->CopyBGRAOffscreenPipelineState
						: Payload->CopyOffscreenPipelineState));
		}
		if (PipelineState == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return;
		}

		CommandList.SetGraphicsPipelineState(*PipelineState);
		CommandList.SetViewport(
			0.0f,
			0.0f,
			0.0f,
			static_cast<float>(Width),
			static_cast<float>(Height),
			1.0f);
		CommandList.SetScissor(
			0.0f,
			0.0f,
			static_cast<float>(Width),
			static_cast<float>(Height));
		CommandList.BindVertexBuffer(
			0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(),
			0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(),
			0);

		FPostProcessViewUniform ViewUniform;
		ViewUniform.InvRenderTargetSize = FVector2f(
			1.0f / static_cast<float>(Width),
			1.0f / static_cast<float>(Height));
		ViewUniform.ExposureScale =
			DisplayMapping::CalculateExposureScale(ExposureEV);
		const FRHIUniformBufferRange ViewUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&ViewUniform,
				sizeof(ViewUniform));

		if (bEnableFXAA)
		{
			FFXAAFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SceneColor = SceneColor;
			FragmentParameters.SceneColorSampler =
				Payload->SceneColorSampler;
			FragmentParameters.View = ViewUniformBuffer;
			SetShaderParameters(
				CommandList,
				Payload->FXAAFragmentShader.GetShaderRef(),
				FragmentParameters);
		}
		else
		{
			FCopySceneColorFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SceneColor = SceneColor;
			FragmentParameters.SceneColorSampler =
				Payload->SceneColorSampler;
			FragmentParameters.View = ViewUniformBuffer;
			SetShaderParameters(
				CommandList,
				Payload->CopyFragmentShader.GetShaderRef(),
				FragmentParameters);
		}

		CommandList.DrawIndexed(3, 0, 0);
	}

	auto FPostProcessRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
