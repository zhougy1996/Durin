#include "Renderers/GBufferDebugRenderer.h"

#include "RenderResourceCreation.h"
#include "RendererResourceSlotCache.h"
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
#include <cstddef>
#include <string>

namespace Durin
{
	namespace
	{
		class FGBufferDebugVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FGBufferDebugVertexShader, FShader,
				"/Engine/GBufferDebug", EShaderFrequency::Vertex, "VertexMain");
		};

		class FGBufferDebugFragmentShader final : public FShader
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
			DURIN_DECLARE_SHADER(FGBufferDebugFragmentShader, FShader,
				"/Engine/GBufferDebug", EShaderFrequency::Fragment,
				"DebugFragmentMain");
		};

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
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FGBufferDebugVertexShader> VertexShader;
			TShaderRef<FGBufferDebugFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> TargetsBySize{
			ERenderResourceGenerationDependency::Device};
	};

	FGBufferDebugRenderer::FGBufferDebugRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FGBufferDebugRenderer::~FGBufferDebugRenderer() = default;

	auto FGBufferDebugRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"GBufferDebugColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate;
				Candidate.Color = RHICreateTexture(Desc);
				if (Candidate.Color == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GBufferDebugTarget", std::to_string(Key),
						"On-demand debug target creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (Targets == nullptr) return nullptr;
		while (State->TargetsBySize.Num() > 1)
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained != nullptr ? Retained->Slot.GetPayload() : nullptr;
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
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType =
					FGBufferDebugVertexShader::StaticType();
				FShaderType& FragmentType =
					FGBufferDebugFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> Types{
					&VertexType, &FragmentType};
				FPayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(
						Types, CompileOptions, Error))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GBufferDebug", "debug", std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				auto* Vertex = static_cast<FGBufferDebugVertexShader*>(
					Candidate.ShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FGBufferDebugFragmentShader*>(
					Candidate.ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"GBufferDebug", "debug",
						"Compiled shader map is missing a typed shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};

				FVertexDeclarationElementList Elements;
				constexpr uint32 Stride =
					sizeof(FFullscreenGeometryResources::FVertex);
				Elements[0] = FVertexElement(0,
					offsetof(FFullscreenGeometryResources::FVertex, Position),
					EVertexElementType::Float2, 0, Stride);
				Elements[1] = FVertexElement(0,
					offsetof(FFullscreenGeometryResources::FVertex, UV),
					EVertexElementType::Float2, 1, Stride);
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);
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
				if (Candidate.VertexDeclaration == nullptr
					|| VertexRHI == nullptr || FragmentRHI == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GBufferDebug", "debug",
						"RHI shader or declaration creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}

				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferDebugOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration = Candidate.VertexDeclaration;
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
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
			ReportRendererResourceCreateDiagnostic);
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

		const std::array DepthReadTransition{
			FRHITextureTransition::Whole(Depth,
				ERHIAccess::DepthStencilReadWrite,
				ERHIAccess::GraphicsShaderRead)};
		CommandList.TransitionTextures(DepthReadTransition);

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
		SetShaderParameters(CommandList, Payload->FragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();

		const std::array DepthWriteTransition{
			FRHITextureTransition::Whole(Depth,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::DepthStencilReadWrite)};
		CommandList.TransitionTextures(DepthWriteTransition);
		return true;
	}

	auto FGBufferDebugRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Slot.Reset();
		State->TargetsBySize.Reset();
	}
} // namespace Durin
