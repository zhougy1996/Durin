#include "Renderers/SkyBoxRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "SkyBoxRendering.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FSkyBoxVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FSkyBoxVertexShader,
				FShader,
				"/Engine/SkyBox",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FSkyBoxFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkyBoxFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SkyTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(SkySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Sky);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FSkyBoxFragmentShader,
				FShader,
				"/Engine/SkyBox",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};
	} // namespace

	struct FSkyBoxRenderer::FState
	{
		struct FPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkyBoxVertexShader> VertexShader;
			TShaderRef<FSkyBoxFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FSkyBoxRenderer::FSkyBoxRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, State(std::make_unique<FState>())
	{
	}

	FSkyBoxRenderer::~FSkyBoxRenderer() = default;

	auto FSkyBoxRenderer::EnsureResources_RenderThread() -> bool
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
					FSkyBoxVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FSkyBoxFragmentShader::StaticType();
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
							"SkyBox",
							"default",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				auto* VertexShader = static_cast<FSkyBoxVertexShader*>(
					ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader = static_cast<FSkyBoxFragmentShader*>(
					ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"SkyBox",
							"default",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				FPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				Candidate.VertexShader = TShaderRef<FSkyBoxVertexShader>(
					VertexShader,
					Candidate.ShaderMap.get());
				Candidate.FragmentShader = TShaderRef<FSkyBoxFragmentShader>(
					FragmentShader,
					Candidate.ShaderMap.get());

				const FVertexDeclarationElementList EmptyVertexElements{};
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(
						EmptyVertexElements);

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
				Initializer.bEnableBackFaceCulling = false;
				Initializer.bEnableDepthTest = false;
				Initializer.bEnableDepthWrite = false;
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"SkyBoxPipeline",
						Initializer);

				const std::array<uint32, 3> FullscreenIndices = {0, 1, 2};
				FRHIBufferCreateDesc IndexBufferDesc =
					FRHIBufferCreateDesc::CreateIndex(
						"SkyBoxFullscreenIndexBuffer",
						sizeof(FullscreenIndices),
						sizeof(uint32));
				IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
				IndexBufferDesc.InitialData = {
					FullscreenIndices.data(),
					sizeof(FullscreenIndices)};
				Candidate.IndexBuffer = RHICreateBuffer(IndexBufferDesc);
				Candidate.Sampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				if (Candidate.VertexDeclaration == nullptr
					|| Candidate.PipelineState == nullptr
					|| Candidate.IndexBuffer == nullptr
					|| Candidate.Sampler == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SkyBox",
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

	auto FSkyBoxRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FSkyBoxSceneData& SkyBox) -> void
	{
		FRHITexture* Texture = SkyBox.TextureReference != nullptr
			? SkyBox.TextureReference->GetReferencedTexture_RenderThread()
			: nullptr;
		if (Texture == nullptr)
		{
			Texture = DefaultTextures.GetCube_RenderThread();
		}
		DrawTexture_RenderThread(CommandList, View, Texture, SkyBox);
	}

	auto FSkyBoxRenderer::DrawTexture_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FRHITexture* Texture,
		const FSkyBoxSceneData& SkyBox) -> void
	{
		const FState::FPayload* Payload = State->Slot.GetPayload();
		if (Texture == nullptr || Payload == nullptr
			|| Payload->PipelineState == nullptr
			|| Payload->Sampler == nullptr
			|| !Payload->VertexShader
			|| !Payload->FragmentShader
			|| Payload->IndexBuffer == nullptr)
		{
			return;
		}

		SkyBoxRendering::FSkyBoxUniform Uniform;
		if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform))
		{
			return;
		}

		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.BindIndexBuffer(Payload->IndexBuffer, 0);
		FSkyBoxFragmentShader::FParameters Parameters;
		Parameters.SkyTexture = Texture;
		Parameters.SkySampler = Payload->Sampler;
		Parameters.Sky = CommandList.AllocateDynamicUniformBuffer(
			&Uniform,
			sizeof(Uniform));
		SetShaderParameters(
			CommandList,
			Payload->FragmentShader,
			Parameters);
		CommandList.DrawIndexed(3, 0, 0);
	}

	auto FSkyBoxRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
