#include "Renderers/PostProcessRenderer.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
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
			FVector2f Padding{0.0f, 0.0f};
		};

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
			FVertexDeclarationRHIRef VertexDeclaration;
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
		TRendererResourceSlotCache<uint64, FSceneTargets> SceneTargetsBySize{
			ERenderResourceGenerationDependency::Device};
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

				FVertexDeclarationElementList VertexDeclElements;
				constexpr uint32 VertexStride =
					sizeof(FFullscreenGeometryResources::FVertex);
				VertexDeclElements[0] = FVertexElement(
					0,
					offsetof(
						FFullscreenGeometryResources::FVertex,
						Position),
					EVertexElementType::Float2,
					0,
					VertexStride);
				VertexDeclElements[1] = FVertexElement(
					0,
					offsetof(FFullscreenGeometryResources::FVertex, UV),
					EVertexElementType::Float2,
					1,
					VertexStride);
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(
						VertexDeclElements);
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
				if (Candidate.VertexDeclaration == nullptr
					|| Candidate.SceneColorSampler == nullptr
					|| CopyVertexRHI == nullptr || CopyFragmentRHI == nullptr
					|| FXAAVertexRHI == nullptr || FXAAFragmentRHI == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"PostProcess",
							"copy+fxaa",
							"RHI shader, declaration, or sampler creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				auto MakePipeline =
					[&Candidate](
						FName Name,
						FRHIShader* VertexShader,
						FRHIShader* FragmentShader,
						const FPipelineLayoutDesc& Layout,
						const FRHIRenderTargetLayout& RenderTargetLayout) {
						return CreatePostProcessPipeline(
							Name,
							VertexShader,
							FragmentShader,
							Candidate.VertexDeclaration,
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
		uint32 Height) -> FSceneTargets*
	{
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		FRHITextureCreateDesc SceneColorDesc =
			FRHITextureCreateDesc::Create2D(
				"SceneColor",
				Width,
				Height,
				EPixelFormat::SRGBA8_UNORM);
		SceneColorDesc.SetFlags(
			ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource);
		SceneColorDesc.SetClearValue(
			FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		FRHITextureCreateDesc SceneDepthDesc =
			FRHITextureCreateDesc::Create2D(
				"SceneDepth",
				Width,
				Height,
				EPixelFormat::D32);
		SceneDepthDesc.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
		SceneDepthDesc.SetClearValue(FClearValueBinding(0.0f, 0u));

		using FResult = TRenderResourceCreateResult<FSceneTargets>;
		auto& Entry = State->SceneTargetsBySize.FindOrAdd(Key);
		FSceneTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[Key, &SceneColorDesc, &SceneDepthDesc]() -> FResult {
				FSceneTargets Candidate;
				Candidate.Color = RHICreateTexture(SceneColorDesc);
				if (Candidate.Color != nullptr)
				{
					Candidate.Depth = RHICreateTexture(SceneDepthDesc);
				}
				if (Candidate.Color == nullptr || Candidate.Depth == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"PostProcessSceneTargets",
							std::to_string(Key),
							"Scene color or depth texture creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		const bool bResolved = Targets != nullptr;
		// Interactive viewport resizing can produce many transient dimensions.
		// Retain stable main and preview sizes without keeping every drag size.
		if (State->SceneTargetsBySize.Num() > 8)
		{
			State->SceneTargetsBySize.EvictOldestExcept(Key);
		}
		auto* StableEntry = State->SceneTargetsBySize.Find(Key);
		return bResolved && StableEntry != nullptr
			? StableEntry->Slot.GetPayload()
			: nullptr;
	}

	auto FPostProcessRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		uint32 Width,
		uint32 Height,
		bool bPresentOutput,
		bool bEnableFXAA,
		bool bHasEditorAssistance) -> void
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

		if (bEnableFXAA)
		{
			FPostProcessViewUniform ViewUniform;
			ViewUniform.InvRenderTargetSize = FVector2f(
				1.0f / static_cast<float>(Width),
				1.0f / static_cast<float>(Height));
			const FRHIUniformBufferRange ViewUniformBuffer =
				CommandList.AllocateDynamicUniformBuffer(
					&ViewUniform,
					sizeof(ViewUniform));

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
		State->SceneTargetsBySize.Reset();
	}
} // namespace Durin
