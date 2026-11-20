#include "Renderers/SkyBoxRenderer.h"

#include "RenderPipelineCreation.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RenderTargetLayouts.h"
#include "SkyBoxRendering.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FSkyBoxVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(
				FSkyBoxVertexShader,
				FGlobalShader,
				"/Engine/SkyBox",
				EShaderFrequency::Vertex,
				"VertexMain"
			);
		};

		class FSkyBoxFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkyBoxFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SkyTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(SkySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Sky);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FSkyBoxFragmentShader,
				FGlobalShader,
				"/Engine/SkyBox",
				EShaderFrequency::Fragment,
				"FragmentMain"
			);
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSkyBoxVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSkyBoxFragmentShader);
		const FGlobalShaderSetRegistration GSkyBoxShaderSet(
			"Renderer", "SkyBox.Default",
			EShaderRequestEligibility::GameAndEditor,
			{&FSkyBoxVertexShader::StaticType(),
			 &FSkyBoxFragmentShader::StaticType()});
	} // namespace

	struct FSkyBoxRenderer::FState
	{
		struct FPayload
		{
			TShaderMapRef<FSkyBoxVertexShader> VertexShader;
			TShaderMapRef<FSkyBoxFragmentShader> FragmentShader;
			FGraphicsPipelineStateInitializer PipelineInitializer;
			FGraphicsPipelineStateInitializer HybridBootstrapPipelineInitializer;
		};

		FVertexDeclarationRHIRef VertexDeclaration;
		FSamplerRHIRef Sampler;
		FBufferRHIRef IndexBuffer;
		std::optional<FPayload> Resources;
	};

	FSkyBoxRenderer::FSkyBoxRenderer(
		FDefaultTextureResources& InDefaultTextures
	)
		: DefaultTextures(InDefaultTextures)
		, State(std::make_unique<FState>())
	{
	}

	FSkyBoxRenderer::~FSkyBoxRenderer() = default;

	auto FSkyBoxRenderer::EnsureResources_RenderThread() -> bool
	{
		using FPayload = FState::FPayload;

		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FSkyBoxVertexShader>();
		const auto Fragment = Map.GetShader<FSkyBoxFragmentShader>();
		State->Resources.reset();
		FPayload Candidate;
		Candidate.VertexShader = Vertex;
		Candidate.FragmentShader = Fragment;
		FRHIShader* VertexRHI =
			Candidate.VertexShader.GetRHIShader(false);
		FRHIShader* FragmentRHI =
			Candidate.FragmentShader.GetRHIShader(false);
		if (VertexRHI == nullptr || FragmentRHI == nullptr)
		{
			return false;
		}

		if (!State->VertexDeclaration)
			State->VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration({});

		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout =
			RenderTargetLayouts::MakeGraphSceneTargets();
		Initializer.BoundShaders.VertexShader =
			VertexRHI;
		Initializer.BoundShaders.FragmentShader =
			FragmentRHI;
		Initializer.VertexDeclaration =
			State->VertexDeclaration;
		Initializer.RasterizerState = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
		Initializer.PipelineLayout =
			Fragment.GetPipelineLayout();
		Candidate.PipelineInitializer = Initializer;
		Initializer.RenderTargetLayout =
			RenderTargetLayouts::MakeGraphHybridSceneBootstrap();
		Initializer.DepthStencilState = RHICreateDepthStencilState({
			.bEnableTest = true,
			.bEnableWrite = false,
			.CompareOp = ERHIDepthCompareOp::Equal});
		Candidate.HybridBootstrapPipelineInitializer = Initializer;

		if (!State->IndexBuffer)
		{
			const std::array<uint32, 3> FullscreenIndices = {0, 1, 2};
			FRHIBufferCreateDesc IndexBufferDesc =
				FRHIBufferCreateDesc::CreateIndex(
					"SkyBoxFullscreenIndexBuffer",
					sizeof(FullscreenIndices),
					sizeof(uint32)
				);
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData = {
				FullscreenIndices.data(),
				sizeof(FullscreenIndices)
			};
			State->IndexBuffer = RHICreateBuffer(IndexBufferDesc);
		}
		if (!State->Sampler) State->Sampler =
			RHICreateSampler(FRHISamplerDesc::LinearClamp());
		if (!State->VertexDeclaration || !State->IndexBuffer || !State->Sampler) return false;
		const auto& Payload = State->Resources.emplace(std::move(Candidate));
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto PipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload.PipelineInitializer, "SkyBoxPipeline");
			if (!PipelineState) return false;
			FRenderPipelinePreparationBatch::Add(PipelineState);
			if (PipelineState->GetState() == ERHIPipelineState::Failed || PipelineState->GetState() == ERHIPipelineState::Canceled) return false;
			auto HybridBootstrapPipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload.HybridBootstrapPipelineInitializer, "SkyBoxHybridBootstrapPipeline");
			if (!HybridBootstrapPipelineState) return false;
			FRenderPipelinePreparationBatch::Add(HybridBootstrapPipelineState);
			if (HybridBootstrapPipelineState->GetState() == ERHIPipelineState::Failed || HybridBootstrapPipelineState->GetState() == ERHIPipelineState::Canceled) return false;
		}
		return true;
	}

	auto FSkyBoxRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FSkyBoxSceneData& SkyBox,
		bool bHybridBootstrap
	) -> void
	{
		FRHITexture* Texture = SkyBox.TextureReference != nullptr ? SkyBox.TextureReference->GetReferencedTexture_RenderThread() : nullptr;
		if (Texture == nullptr)
		{
			Texture = DefaultTextures.GetCube_RenderThread();
		}
		DrawTexture_RenderThread(
			CommandList, View, Texture, SkyBox, bHybridBootstrap
		);
	}

	auto FSkyBoxRenderer::DrawTexture_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FRHITexture* Texture,
		const FSkyBoxSceneData& SkyBox,
		bool bHybridBootstrap
	) -> bool
	{
		const auto& Payload = State->Resources;
		const auto* Pipeline = Payload ? &(bHybridBootstrap ? Payload->HybridBootstrapPipelineInitializer : Payload->PipelineInitializer) : nullptr;
		if (Texture == nullptr || !Payload
			|| !Pipeline
			|| State->Sampler == nullptr
			|| !Payload->VertexShader
			|| !Payload->FragmentShader
			|| State->IndexBuffer == nullptr)
		{
			return false;
		}

		SkyBoxRendering::FSkyBoxUniform Uniform;
		if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform))
		{
			return false;
		}

		SetGraphicsPipelineState(CommandList, *Pipeline);
		CommandList.BindIndexBuffer(State->IndexBuffer, 0);
		FSkyBoxFragmentShader::FParameters Parameters;
		Parameters.SkyTexture = Texture;
		Parameters.SkySampler = State->Sampler;
		Parameters.Sky = CommandList.CreateUniformBufferRange(
			&Uniform,
			sizeof(Uniform)
		);
		SetShaderParameters(
			CommandList,
			Payload->FragmentShader,
			Parameters
		);
		CommandList.DrawIndexed(3, 0, 0);
		return true;
	}

	auto FSkyBoxRenderer::ReleaseResources_RenderThread() -> void
	{
		*State = {};
	}
} // namespace Durin
