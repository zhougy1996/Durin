#include "Renderers/TextureCubeThumbnailRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "CoreGlobals.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		class FTextureCubeThumbnailVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(
				FTextureCubeThumbnailVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FTextureCubeThumbnailVertexShader,
				FShader,
				"/Engine/TextureCubeThumbnail",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FTextureCubeThumbnailFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(
				FTextureCubeThumbnailFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(CubeTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(CubeSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FTextureCubeThumbnailFragmentShader,
				FShader,
				"/Engine/TextureCubeThumbnail",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};
	} // namespace

	struct FTextureCubeThumbnailRenderer::FState
	{
		struct FPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FTextureCubeThumbnailVertexShader> VertexShader;
			TShaderRef<FTextureCubeThumbnailFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FSamplerRHIRef Sampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FTextureCubeThumbnailRenderer::FTextureCubeThumbnailRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FTextureCubeThumbnailRenderer::~FTextureCubeThumbnailRenderer() = default;

	auto FTextureCubeThumbnailRenderer::EnsureResources_RenderThread() -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;

		FPayload* Payload = State->Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this]() -> FResult {
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FTextureCubeThumbnailVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FTextureCubeThumbnailFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> ShaderTypes = {
					&VertexShaderType,
					&FragmentShaderType};
				auto ShaderMap = std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!ShaderMap->InitializeFromShaderTypes(
						ShaderTypes,
						CompileOptions,
						ErrorMessage))
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"TextureCubeThumbnail",
							"default",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				auto* VertexShader =
					static_cast<FTextureCubeThumbnailVertexShader*>(
						ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader =
					static_cast<FTextureCubeThumbnailFragmentShader*>(
						ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"TextureCubeThumbnail",
							"default",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				FPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				Candidate.VertexShader =
					TShaderRef<FTextureCubeThumbnailVertexShader>(
						VertexShader,
						Candidate.ShaderMap.get());
				Candidate.FragmentShader =
					TShaderRef<FTextureCubeThumbnailFragmentShader>(
						FragmentShader,
						Candidate.ShaderMap.get());
				const FVertexDeclarationElementList VertexDeclElements =
					GetStaticMeshVertexDeclarationElements();
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(
						VertexDeclElements);

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeSceneTargets();
				Initializer.BoundShaders.VertexShader =
					Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration =
					Candidate.VertexDeclaration;
				Initializer.bEnableAlphaBlend = false;
				Initializer.bEnableDepthTest = true;
				Initializer.bEnableDepthWrite = true;
				Initializer.bEnableBackFaceCulling = false;
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"TextureCubeThumbnailPipeline",
						Initializer);
				Candidate.Sampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				if (Candidate.VertexDeclaration == nullptr
					|| Candidate.PipelineState == nullptr
					|| Candidate.Sampler == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"TextureCubeThumbnail",
							"default",
							"RHI resource creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		return Payload != nullptr;
	}

	auto FTextureCubeThumbnailRenderer::DrawProxy_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FTextureCubePreviewSceneProxy& Proxy,
		FSkyBoxRenderer& SkyBoxRenderer) -> void
	{
		const FRHITextureReferenceRef& TextureReference =
			Proxy.GetTextureReference();
		if (TextureReference == nullptr)
		{
			return;
		}
		FRHITexture* Texture =
			TextureReference->GetReferencedTexture_RenderThread();
		if (Texture == nullptr)
		{
			return;
		}

		// Content Browser thumbnails favor recognition over inspection: show a
		// wide environment view here and reserve the reflective sphere for the
		// interactive TextureCube editor.
		constexpr float EnvironmentVerticalFieldOfViewDegrees = 100.0f;
		FSceneView EnvironmentView = View;
		const float AspectRatio = static_cast<float>(View.ViewportWidth)
			/ static_cast<float>(std::max(1u, View.ViewportHeight));
		const float YScale = 1.0f
			/ std::tan(
				glm::radians(EnvironmentVerticalFieldOfViewDegrees) * 0.5f);
		EnvironmentView.ProjectionMatrix[1][0] =
			YScale / std::max(AspectRatio, 0.001f);
		EnvironmentView.ProjectionMatrix[2][1] = -YScale;
		EnvironmentView.ViewProjectionMatrix =
			EnvironmentView.ProjectionMatrix * EnvironmentView.ViewMatrix;

		SkyBoxRenderer.DrawTexture_RenderThread(
			CommandList,
			EnvironmentView,
			Texture,
			FSkyBoxSceneData{});
	}

	auto FTextureCubeThumbnailRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
