#include "Renderers/ContactShadowRenderer.h"

#include "RenderResourceCreation.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace Durin
{
	namespace
	{
		class FContactShadowVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FContactShadowVertexShader,
				FShader,
				"/Engine/ContactShadow",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FContactShadowFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactShadowFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(DirectionalDirect);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FContactShadowFragmentShader,
				FShader,
				"/Engine/ContactShadow",
				EShaderFrequency::Fragment,
				"ContactShadowFragmentMain");
		};

		// Matches ContactShadowUniform in ContactShadow.slang byte-for-byte.
		struct alignas(16) FContactShadowUniform
		{
			float InverseViewProjection[16]{};
			float ViewProjection[16]{};
			// Cover the directional shadow's maximum 0.08-world-unit bias while
			// remaining a bounded near-field supplement. A small start offset keeps
			// the first useful sample close enough to refill foot and wall seams.
			float ToLight[4]{0.0f, 0.0f, 0.0f, 0.75f};
			float RayThickness = 0.08f;
			float StepCount = 24.0f;
			float StartOffset = 0.01f;
			float bReversedZ = 0.0f;
			float InvViewportX = 1.0f;
			float InvViewportY = 1.0f;
			float bShowDebug = 0.0f;
			float MaxScreenDistancePixels = 96.0f;
		};
		static_assert(sizeof(FContactShadowUniform) == 176);

		auto CreateContactShadowPipeline(
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

	struct FScreenSpaceContactShadowRenderer::FState
	{
		struct FPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FContactShadowVertexShader> VertexShader;
			TShaderRef<FContactShadowFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FSamplerRHIRef SceneColorSampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FScreenSpaceContactShadowRenderer::FScreenSpaceContactShadowRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FScreenSpaceContactShadowRenderer::~FScreenSpaceContactShadowRenderer() = default;

	auto FScreenSpaceContactShadowRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		FRHITexture* DirectionalDirect,
		FRHITexture* SceneDepth,
		FRHITexture* ContactColor,
		const FSceneView& View,
		const FVector3& LightDirection,
		bool bShowDebug,
		uint32 Width,
		uint32 Height) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());

		if (SceneColor == nullptr || DirectionalDirect == nullptr
			|| SceneDepth == nullptr
			|| ContactColor == nullptr || Width == 0 || Height == 0)
			return false;

		const double LightLengthSquared = glm::dot(LightDirection, LightDirection);
		if (!std::isfinite(LightLengthSquared) || LightLengthSquared <= 1.0e-8)
			return false;

		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FContactShadowVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FContactShadowFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> ShaderTypes = {
					&VertexShaderType,
					&FragmentShaderType};

				FPayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(
						ShaderTypes,
						CompileOptions,
						ErrorMessage))
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"ContactShadow",
							"contact-shadow",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				auto* VertexShader = static_cast<FContactShadowVertexShader*>(
					Candidate.ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader =
					static_cast<FContactShadowFragmentShader*>(
						Candidate.ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"ContactShadow",
							"contact-shadow",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				Candidate.VertexShader =
					TShaderRef<FContactShadowVertexShader>(
						VertexShader,
						Candidate.ShaderMap.get());
				Candidate.FragmentShader =
					TShaderRef<FContactShadowFragmentShader>(
						FragmentShader,
						Candidate.ShaderMap.get());

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
					offsetof(
						FFullscreenGeometryResources::FVertex,
						UV),
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
							"ContactShadow",
							"fullscreen-geometry",
							"Shared fullscreen geometry is unavailable.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}

				Candidate.SceneColorSampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				FRHIShader* VertexRHI =
					Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI =
					Candidate.FragmentShader.GetRHIShader(false);
				if (Candidate.VertexDeclaration == nullptr
					|| Candidate.SceneColorSampler == nullptr
					|| VertexRHI == nullptr || FragmentRHI == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"ContactShadow",
							"contact-shadow",
							"RHI shader, declaration, or sampler creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}

				Candidate.PipelineState = CreateContactShadowPipeline(
					"ContactShadowPipeline",
					VertexRHI,
					FragmentRHI,
					Candidate.VertexDeclaration,
					Candidate.ShaderMap->GetMergedPipelineLayout(),
					RenderTargetLayouts::MakeContactShadowOutput());
				if (Candidate.PipelineState == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"ContactShadow",
							"contact-shadow",
							"RHI pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Payload == nullptr)
			return false;

		if (FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
			return false;

		FMatrix InvViewProjection;
		if (!Math::TryInverse(
				View.ViewProjectionMatrix, InvViewProjection, 1.0e-8))
			return false;

		FContactShadowUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InvViewProjection[Col][Row]);
				Uniform.ViewProjection[Row * 4 + Col] =
					static_cast<float>(
						View.ViewProjectionMatrix[Col][Row]);
			}
		}
		const FVector3 ToLight =
			-Math::Normalize(LightDirection);
		Uniform.ToLight[0] = static_cast<float>(ToLight.x);
		Uniform.ToLight[1] = static_cast<float>(ToLight.y);
		Uniform.ToLight[2] = static_cast<float>(ToLight.z);
		Uniform.bReversedZ =
			View.DepthConvention == ESceneDepthConvention::ReversedZ
				? 1.0f : 0.0f;
		Uniform.InvViewportX = 1.0f / static_cast<float>(Width);
		Uniform.InvViewportY = 1.0f / static_cast<float>(Height);
		Uniform.bShowDebug = bShowDebug ? 1.0f : 0.0f;

		const std::array DepthReadTransition{
			FRHITextureTransition::Whole(
				SceneDepth,
				ERHIAccess::DepthStencilReadWrite,
				ERHIAccess::GraphicsShaderRead)};
		CommandList.TransitionTextures(DepthReadTransition);

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeContactShadowOutput();
		PassInfo.ColorRenderTargets[0] = ContactColor;
		CommandList.BeginRenderPass(PassInfo, "ContactShadowRenderPass");

		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
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

		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&Uniform,
				sizeof(Uniform));

		FContactShadowFragmentShader::FParameters FragmentParameters;
		FragmentParameters.SceneColor = SceneColor;
		FragmentParameters.SceneColorSampler =
			Payload->SceneColorSampler;
		FragmentParameters.DirectionalDirect = DirectionalDirect;
		FragmentParameters.SceneDepth = SceneDepth;
		FragmentParameters.Params = UniformBuffer;
		SetShaderParameters(
			CommandList,
			Payload->FragmentShader,
			FragmentParameters);

		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();

		const std::array DepthWriteTransition{
			FRHITextureTransition::Whole(
				SceneDepth,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::DepthStencilReadWrite)};
		CommandList.TransitionTextures(DepthWriteTransition);
		return true;
	}

	auto FScreenSpaceContactShadowRenderer::ReleaseResources_RenderThread()
		-> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
