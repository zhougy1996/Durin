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

		auto GetPostProcessVariantIndex(bool bPresentOutput, bool bEnableFXAA,
			bool bHasEditorAssistance, EPixelFormat OutputFormat) -> size_t
		{
			const size_t Output = bHasEditorAssistance ? 0 : bPresentOutput ? 2 : 1;
			return Output * 4 + (OutputFormat == EPixelFormat::SBGRA8_UNORM ? 2 : 0) + (bEnableFXAA ? 1 : 0);
		}

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
			return FRenderPipelineRequestScope::Graphics(
				PipelineName,
				Initializer);
		}
	} // namespace

	struct FPostProcessRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FPostProcessVertexShader> VertexShader;
			TShaderMapRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderMapRef<FFXAAFragmentShader> FXAAFragmentShader;
			FGraphicsPipelineStateRHIRef Pipeline;
			FSamplerRHIRef SceneColorSampler;
		};
		using FSlot = TRenderResourceCreationSlot<FPayload>;
		// Two shader routes, two formats, and three output attachment layouts.
		std::array<std::unique_ptr<FSlot>, 12> Slots;
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
		FRHICommandListImmediate& CommandList, bool bPresentOutput, bool bEnableFXAA,
		bool bHasEditorAssistance, EPixelFormat OutputFormat) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		auto& Slot = State->Slots[GetPostProcessVariantIndex(bPresentOutput,
			bEnableFXAA, bHasEditorAssistance, OutputFormat)];
		if (!Slot) Slot = std::make_unique<FState::FSlot>(
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device);
		return Slot->Resolve(Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList, bPresentOutput, bEnableFXAA, bHasEditorAssistance, OutputFormat]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> ShaderTypes = {
					&FPostProcessVertexShader::StaticType(),
					bEnableFXAA ? &FFXAAFragmentShader::StaticType() : &FCopySceneColorFragmentShader::StaticType()};
				const auto Completion = GetGlobalShaderMap().ResolveShaderSet(
					bEnableFXAA ? "PostProcess.FXAA" : "PostProcess.Copy", ShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (Completion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
				if (Completion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
				if (!Completion) return FResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::ShaderCompile, "PostProcess", bEnableFXAA ? "fxaa" : "copy",
					ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual));
				FPayload Candidate;
				Candidate.ShaderSet = *Completion;
				Candidate.VertexShader = TShaderMapRef<FPostProcessVertexShader>(Candidate.ShaderSet);
				FRHIShader* FragmentRHI = nullptr;
				if (bEnableFXAA)
				{
					Candidate.FXAAFragmentShader = TShaderMapRef<FFXAAFragmentShader>(Candidate.ShaderSet);
					FragmentRHI = Candidate.FXAAFragmentShader.GetRHIShader(false);
				}
				else
				{
					Candidate.CopyFragmentShader = TShaderMapRef<FCopySceneColorFragmentShader>(Candidate.ShaderSet);
					FragmentRHI = Candidate.CopyFragmentShader.GetRHIShader(false);
				}
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource, "PostProcess", "fullscreen-geometry",
						ERenderResourceCreateErrorReason::FullscreenGeometryUnavailable,
						ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
				Candidate.SceneColorSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				if (!Candidate.SceneColorSampler || !VertexRHI || !FragmentRHI)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource, "PostProcess", bEnableFXAA ? "fxaa" : "copy",
						ERenderResourceCreateErrorReason::ResourceCreationFailed,
						ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				const bool bBGRA = OutputFormat == EPixelFormat::SBGRA8_UNORM;
				const auto Format = bBGRA ? EPixelFormat::SBGRA8_UNORM : EPixelFormat::SRGBA8_UNORM;
				const auto Layout = bHasEditorAssistance
					? RenderTargetLayouts::MakeScenePostProcessOutput(Format)
					: RenderTargetLayouts::MakeFinalScenePostProcessOutput(bPresentOutput
						? RenderTargetLayouts::EViewportOutput::Present : RenderTargetLayouts::EViewportOutput::Offscreen, Format);
				const FName Name(std::format("PostProcess{}{}{}Pipeline", bEnableFXAA ? "FXAA" : "Copy",
					bBGRA ? "BGRA" : "", bHasEditorAssistance ? "Intermediate" : bPresentOutput ? "Present" : "Offscreen"));
				Candidate.Pipeline = CreatePostProcessPipeline(Name, VertexRHI, FragmentRHI,
					FullscreenGeometry.GetVertexDeclaration_RenderThread(), Candidate.ShaderSet.GetPipelineLayout(), Layout);
				if (!Candidate.Pipeline) return FResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "PostProcess", bEnableFXAA ? "fxaa" : "copy",
					ERenderResourceCreateErrorReason::PipelineCreationFailed,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
						| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable) != nullptr;
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
		const auto& Slot = State->Slots[GetPostProcessVariantIndex(bPresentOutput,
			bEnableFXAA, bHasEditorAssistance, OutputFormat)];
		const FState::FPayload* Payload = Slot ? Slot->GetPayload() : nullptr;
		if (!Payload) return;
		const FGraphicsPipelineStateRHIRef& PipelineState = Payload->Pipeline;
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
			CommandList.CreateUniformBufferRange(
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
		for (auto& Slot : State->Slots) Slot.reset();
	}
} // namespace Durin
