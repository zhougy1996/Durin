#include "Renderers/PostProcessRenderer.h"

#include "Renderers/DisplayMapping.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FPostProcessVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FPostProcessVertexShader,
				FShader,
				"/Engine/PostProcess",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FCopySceneColorFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCopySceneColorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FCopySceneColorFragmentShader,
				FShader,
				"/Engine/PostProcess",
				EShaderFrequency::Fragment,
				"CopyFragmentMain");
		};

		class FFXAAFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FFXAAFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FFXAAFragmentShader,
				FShader,
				"/Engine/PostProcess",
				EShaderFrequency::Fragment,
				"FXAAFragmentMain");
		};

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
			std::shared_ptr<FShaderMapBase> CopyShaderMap;
			std::shared_ptr<FShaderMapBase> FXAAShaderMap;
			TShaderRef<FPostProcessVertexShader> CopyVertexShader;
			TShaderRef<FPostProcessVertexShader> FXAAVertexShader;
			TShaderRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderRef<FFXAAFragmentShader> FXAAFragmentShader;
			FGraphicsPipelineStateRHIRef CopyIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef FXAAIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef CopyOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef CopyPresentPipelineState;
			FGraphicsPipelineStateRHIRef FXAAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef FXAAPresentPipelineState;
			FSamplerRHIRef SceneColorSampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FPostProcessRenderer::FPostProcessRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry,
		FRendererTransientTargetPool& InTransientTargets)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, TransientTargets(InTransientTargets)
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
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FPostProcessVertexShader::StaticType();
				FShaderType& CopyFragmentShaderType =
					FCopySceneColorFragmentShader::StaticType();
				FShaderType& FXAAFragmentShaderType =
					FFXAAFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> CopyShaderTypes = {
					&VertexShaderType,
					&CopyFragmentShaderType};
				const std::array<const FShaderType*, 2> FXAAShaderTypes = {
					&VertexShaderType,
					&FXAAFragmentShaderType};

				FPayload Candidate;
				Candidate.CopyShaderMap =
					std::make_shared<FShaderMapBase>();
				Candidate.FXAAShaderMap =
					std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!Candidate.CopyShaderMap->InitializeFromShaderTypes(
						CopyShaderTypes,
						CompileOptions,
						ErrorMessage))
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"copy",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				if (!Candidate.FXAAShaderMap->InitializeFromShaderTypes(
						FXAAShaderTypes,
						CompileOptions,
						ErrorMessage))
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"fxaa",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				auto* CopyVertexShader =
					static_cast<FPostProcessVertexShader*>(
						Candidate.CopyShaderMap->GetShader(
							&VertexShaderType));
				auto* FXAAVertexShader =
					static_cast<FPostProcessVertexShader*>(
						Candidate.FXAAShaderMap->GetShader(
							&VertexShaderType));
				auto* CopyFragmentShader =
					static_cast<FCopySceneColorFragmentShader*>(
						Candidate.CopyShaderMap->GetShader(
							&CopyFragmentShaderType));
				auto* FXAAFragmentShader =
					static_cast<FFXAAFragmentShader*>(
						Candidate.FXAAShaderMap->GetShader(
							&FXAAFragmentShaderType));
				if (CopyVertexShader == nullptr
					|| FXAAVertexShader == nullptr
					|| CopyFragmentShader == nullptr
					|| FXAAFragmentShader == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"PostProcess",
							"copy+fxaa",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				Candidate.CopyVertexShader =
					TShaderRef<FPostProcessVertexShader>(
						CopyVertexShader,
						Candidate.CopyShaderMap.get());
				Candidate.FXAAVertexShader =
					TShaderRef<FPostProcessVertexShader>(
						FXAAVertexShader,
						Candidate.FXAAShaderMap.get());
				Candidate.CopyFragmentShader =
					TShaderRef<FCopySceneColorFragmentShader>(
						CopyFragmentShader,
						Candidate.CopyShaderMap.get());
				Candidate.FXAAFragmentShader =
					TShaderRef<FFXAAFragmentShader>(
						FXAAFragmentShader,
						Candidate.FXAAShaderMap.get());

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
					Candidate.CopyShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput());
				Candidate.FXAAIntermediatePipelineState = MakePipeline(
					"PostProcessFXAAIntermediatePipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeScenePostProcessOutput());
				Candidate.CopyOffscreenPipelineState = MakePipeline(
					"PostProcessCopyOffscreenPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen));
				Candidate.CopyPresentPipelineState = MakePipeline(
					"PostProcessCopyPresentPipeline",
					CopyVertexRHI,
					CopyFragmentRHI,
					Candidate.CopyShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present));
				Candidate.FXAAOffscreenPipelineState = MakePipeline(
					"PostProcessFXAAOffscreenPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen));
				Candidate.FXAAPresentPipelineState = MakePipeline(
					"PostProcessFXAAPresentPipeline",
					FXAAVertexRHI,
					FXAAFragmentRHI,
					Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Present));
				if (Candidate.CopyIntermediatePipelineState == nullptr
					|| Candidate.FXAAIntermediatePipelineState == nullptr
					|| Candidate.CopyOffscreenPipelineState == nullptr
					|| Candidate.CopyPresentPipelineState == nullptr
					|| Candidate.FXAAOffscreenPipelineState == nullptr
					|| Candidate.FXAAPresentPipelineState == nullptr)
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
			ReportRendererResourceCreateDiagnostic);
		return Payload != nullptr;
	}

	auto FPostProcessRenderer::EnsureSceneTargets_RenderThread(
		uint32 Width,
		uint32 Height) -> std::optional<FSceneTargets>
	{
		if (Width == 0 || Height == 0) return std::nullopt;
		const std::array Descriptions{
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
		auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::Scene, Descriptions,
			MaximumRetainedSceneTargetBytes);
		if (!Lease) return std::nullopt;
		return FSceneTargets{
			.Color = Lease->Textures[0], .Depth = Lease->Textures[1]};
	}
	auto FPostProcessRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		uint32 Width,
		uint32 Height,
		bool bPresentOutput,
		bool bEnableFXAA,
		bool bHasEditorAssistance,
		float ExposureEV) -> void
	{
		const FState::FPayload* Payload = State->Slot.GetPayload();
		if (Payload == nullptr)
		{
			return;
		}

		FGraphicsPipelineStateRHIRef PipelineState;
		if (bHasEditorAssistance)
		{
			PipelineState = bEnableFXAA
				? Payload->FXAAIntermediatePipelineState
				: Payload->CopyIntermediatePipelineState;
		}
		else
		{
			PipelineState = bEnableFXAA
				? (bPresentOutput
					? Payload->FXAAPresentPipelineState
					: Payload->FXAAOffscreenPipelineState)
				: (bPresentOutput
					? Payload->CopyPresentPipelineState
					: Payload->CopyOffscreenPipelineState);
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
				Payload->FXAAFragmentShader,
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
				Payload->CopyFragmentShader,
				FragmentParameters);
		}

		CommandList.DrawIndexed(3, 0, 0);
	}

	auto FPostProcessRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
