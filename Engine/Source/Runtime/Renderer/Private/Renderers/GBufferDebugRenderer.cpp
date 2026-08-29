#include "Renderers/GBufferDebugRenderer.h"

#include "RenderResourceCreation.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Math/Operations.h"
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
		class FGBufferDebugVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FGBufferDebugVertexShader, FGlobalShader,
				"/Engine/GBufferDebug", EShaderFrequency::Vertex, "VertexMain");
		};

		class FGBufferDebugFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferDebugFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FGBufferDebugFragmentShader, FGlobalShader,
				"/Engine/GBufferDebug", EShaderFrequency::Fragment,
				"DebugFragmentMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGBufferDebugVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGBufferDebugFragmentShader);

		struct alignas(16) FGBufferDebugUniform
		{
			float ProjectionRows[16]{};
			float InverseProjection[16]{};
			float Mode = 0.0f;
			float InverseViewportX = 1.0f;
			float InverseViewportY = 1.0f;
			float PositionScale = 0.01f;
		};
		static_assert(sizeof(FGBufferDebugUniform) == 144);
	} // namespace

	struct FGBufferDebugRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FGBufferDebugVertexShader> VertexShader;
			TShaderMapRef<FGBufferDebugFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FGBufferDebugRenderer::FGBufferDebugRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry,
		FRendererTransientTargetPool& InTransientTargets)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, TransientTargets(InTransientTargets)
		, State(std::make_unique<FState>())
	{
	}

	FGBufferDebugRenderer::~FGBufferDebugRenderer() = default;

	auto FGBufferDebugRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> std::optional<FTargets>
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0) return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"GBufferDebugColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		const uint64 RetainedBudget = static_cast<uint64>(Width) * Height * 16;
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::GBufferDebug,
			std::span(&Desc, 1), RetainedBudget);
		if (!Lease) return std::nullopt;
		return FTargets{.Color = Lease->Textures[0]};
	}

	auto FGBufferDebugRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* Material,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Emissive,
		FRHITexture* Depth,
		FRHITexture* Output,
		const FSceneView& View,
		EGBufferDebugMode Mode,
		uint32 Width,
		uint32 Height) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Material == nullptr || Normals == nullptr || Surface == nullptr
			|| Emissive == nullptr || Depth == nullptr || Output == nullptr
			|| Width == 0 || Height == 0
			|| Mode == EGBufferDebugMode::Disabled
			|| Mode >= EGBufferDebugMode::Count)
		{
			return false;
		}

		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> Types{
					&FGBufferDebugVertexShader::StaticType(),
					&FGBufferDebugFragmentShader::StaticType()};
				FPayload Candidate;
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"GBufferDebug.Debug", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GBufferDebug", "debug", "Global shader set is unavailable.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader = TShaderMapRef<FGBufferDebugVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FGBufferDebugFragmentShader>(Candidate.ShaderSet);

				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GBufferDebug", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				FRHIShader* VertexRHI =
					Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI =
					Candidate.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GBufferDebug", "debug",
						"RHI shader creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferDebugOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GBufferDebugPipeline", Initializer);
				if (Candidate.PipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GBufferDebug", "debug",
						"RHI pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		if (Payload == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return false;
		}

		FMatrix InverseProjection;
		if (!Math::TryInverse(View.ProjectionMatrix, InverseProjection, 1.0e-12))
			return false;
		FGBufferDebugUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row * 4 + Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
				Uniform.InverseProjection[Row * 4 + Col] =
					static_cast<float>(InverseProjection[Col][Row]);
			}
		}
		Uniform.Mode = static_cast<float>(Mode);
		Uniform.InverseViewportX = 1.0f / static_cast<float>(Width);
		Uniform.InverseViewportY = 1.0f / static_cast<float>(Height);

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeGBufferDebugOutput();
		PassInfo.ColorRenderTargets[0] = Output;
		CommandList.BeginRenderPass(PassInfo, "GBufferDebugRenderPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(0.0f, 0.0f, 0.0f,
			static_cast<float>(Width), static_cast<float>(Height), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f,
			static_cast<float>(Width), static_cast<float>(Height));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);

		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		FGBufferDebugFragmentShader::FParameters Parameters;
		Parameters.GBufferMaterial = Material;
		Parameters.GBufferNormals = Normals;
		Parameters.GBufferSurface = Surface;
		Parameters.GBufferEmissive = Emissive;
		Parameters.SceneDepth = Depth;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->FragmentShader.GetShaderRef(), Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();

		return true;
	}

	auto FGBufferDebugRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
	}
} // namespace Durin
