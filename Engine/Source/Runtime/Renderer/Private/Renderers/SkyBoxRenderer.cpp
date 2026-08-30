#include "Renderers/SkyBoxRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RendererResourceCoordinator.h"
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
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FSkyBoxVertexShader> VertexShader;
			TShaderMapRef<FSkyBoxFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FGraphicsPipelineStateRHIRef HybridBootstrapPipelineState;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
	};

	FSkyBoxRenderer::FSkyBoxRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures
	)
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
				const std::array<const FGlobalShaderType*, 2> ShaderTypes = {
					&FSkyBoxVertexShader::StaticType(),
					&FSkyBoxFragmentShader::StaticType()};
				FPayload Candidate;
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"SkyBox.Default", ShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"SkyBox",
							"default",
							"Global shader set is unavailable.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}

				Candidate.VertexShader = TShaderMapRef<FSkyBoxVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FSkyBoxFragmentShader>(Candidate.ShaderSet);
				FRHIShader* VertexRHI =
					Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI =
					Candidate.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SkyBox",
							"default",
							"RHI shader creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}

				const FVertexDeclarationElementList EmptyVertexElements{};
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(
						EmptyVertexElements
					);

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeSceneTargets();
				Initializer.BoundShaders.VertexShader =
					VertexRHI;
				Initializer.BoundShaders.FragmentShader =
					FragmentRHI;
				Initializer.VertexDeclaration =
					Candidate.VertexDeclaration;
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"SkyBoxPipeline",
						Initializer
					);
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeHybridSceneBootstrap();
				Candidate.HybridBootstrapPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"SkyBoxHybridBootstrapPipeline", Initializer
					);

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
				Candidate.IndexBuffer = RHICreateBuffer(IndexBufferDesc);
				Candidate.Sampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				if (Candidate.VertexDeclaration == nullptr
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
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}
				if (Candidate.PipelineState == nullptr
					|| Candidate.HybridBootstrapPipelineState == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"SkyBox",
							"default",
							"RHI graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable
		);
		return Payload != nullptr;
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
		const FState::FPayload* Payload = State->Slot.GetPayload();
		FRHIGraphicsPipelineState* Pipeline = Payload != nullptr ? (bHybridBootstrap ? Payload->HybridBootstrapPipelineState.GetReference() : Payload->PipelineState.GetReference()) : nullptr;
		if (Texture == nullptr || Payload == nullptr
			|| Pipeline == nullptr
			|| Payload->Sampler == nullptr
			|| !Payload->VertexShader
			|| !Payload->FragmentShader
			|| Payload->IndexBuffer == nullptr)
		{
			return false;
		}

		SkyBoxRendering::FSkyBoxUniform Uniform;
		if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform))
		{
			return false;
		}

		CommandList.SetGraphicsPipelineState(*Pipeline);
		CommandList.BindIndexBuffer(Payload->IndexBuffer, 0);
		FSkyBoxFragmentShader::FParameters Parameters;
		Parameters.SkyTexture = Texture;
		Parameters.SkySampler = Payload->Sampler;
		Parameters.Sky = CommandList.AllocateDynamicUniformBuffer(
			&Uniform,
			sizeof(Uniform)
		);
		SetShaderParameters(
			CommandList,
			Payload->FragmentShader.GetShaderRef(),
			Parameters
		);
		CommandList.DrawIndexed(3, 0, 0);
		return true;
	}

	auto FSkyBoxRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
