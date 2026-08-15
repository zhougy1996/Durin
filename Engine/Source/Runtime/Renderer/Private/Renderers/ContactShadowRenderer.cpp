#include "Renderers/ContactShadowRenderer.h"

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
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace Durin
{
	namespace
	{
		class FContactVisibilityVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FContactVisibilityVertexShader, FShader,
				"/Engine/ContactShadow", EShaderFrequency::Vertex, "VertexMain");
		};

		class FContactVisibilityFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactVisibilityFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FContactVisibilityFragmentShader, FShader,
				"/Engine/ContactShadow", EShaderFrequency::Fragment,
				"ContactVisibilityFragmentMain");
		};

		struct alignas(16) FContactVisibilityUniform
		{
			float InverseViewProjection[16]{};
			float ViewProjection[16]{};
			float ToLightMaxDistance[4]{0.0f, 0.0f, 0.0f, 0.20f};
			float ThicknessStepsOffsetReversedZ[4]{0.012f, 16.0f, 0.01f, 0.0f};
			float Viewport[4]{1.0f, 1.0f, 0.0f, 0.0f};
			float Trace[4]{48.0f, 0.0f, 0.0f, 0.0f};
		};
		static_assert(sizeof(FContactVisibilityUniform) == 192);
	} // namespace

	struct FContactShadowVisibilityRenderer::FState
	{
		struct FPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FContactVisibilityVertexShader> VertexShader;
			TShaderRef<FContactVisibilityFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FPayload> Resources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> TargetsBySize{
			ERenderResourceGenerationDependency::Device};
	};

	FContactShadowVisibilityRenderer::FContactShadowVisibilityRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
		  State(std::make_unique<FState>()) {}

	FContactShadowVisibilityRenderer::~FContactShadowVisibilityRenderer() = default;

	auto FContactShadowVisibilityRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"DirectionalContactVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate;
				Candidate.Visibility = RHICreateTexture(Desc);
				if (Candidate.Visibility == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibilityTarget", std::to_string(Key),
						"R8_UNORM visibility target creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		const bool bResolved = Targets != nullptr;
		auto GetRetainedBytes = [this]() {
			return State->TargetsBySize.GetRetainedPayloadWeight(
				[](uint64 SizeKey, const FTargets&) {
					return CalculateTargetBytes(static_cast<uint32>(SizeKey >> 32),
						static_cast<uint32>(SizeKey));
				});
		};
		while (State->TargetsBySize.Num() > 1
			&& GetRetainedBytes() > MaximumRetainedBytes)
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		if (!bResolved) return nullptr;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained != nullptr ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FContactShadowVisibilityRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList, FTargets& Targets,
		FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface,
		FRHITexture* Emissive, FRHITexture* SceneDepth, const FSceneView& View,
		const FVector3& LightDirection, uint32 Width, uint32 Height) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Visibility == nullptr || Material == nullptr || Normals == nullptr
			|| Surface == nullptr || Emissive == nullptr || SceneDepth == nullptr
			|| Width == 0 || Height == 0 || View.ViewportWidth == 0
			|| View.ViewportHeight == 0) return false;
		const double LightLengthSquared = glm::dot(LightDirection, LightDirection);
		if (!std::isfinite(LightLengthSquared) || LightLengthSquared <= 1.0e-8)
			return false;

		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType = FContactVisibilityVertexShader::StaticType();
				FShaderType& FragmentType = FContactVisibilityFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> Types{&VertexType, &FragmentType};
				FPayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"ContactVisibility", "shader", std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				auto* Vertex = static_cast<FContactVisibilityVertexShader*>(
					Candidate.ShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FContactVisibilityFragmentShader*>(
					Candidate.ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"ContactVisibility", "shader",
						"Compiled shader map is missing a typed shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
				FVertexDeclarationElementList Elements;
				constexpr uint32 Stride = sizeof(FFullscreenGeometryResources::FVertex);
				Elements[0] = FVertexElement(0, offsetof(
					FFullscreenGeometryResources::FVertex, Position),
					EVertexElementType::Float2, 0, Stride);
				Elements[1] = FVertexElement(0, offsetof(
					FFullscreenGeometryResources::FVertex, UV),
					EVertexElementType::Float2, 1, Stride);
				Candidate.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibility", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				if (Candidate.VertexDeclaration == nullptr || VertexRHI == nullptr
					|| FragmentRHI == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibility", "pipeline",
						"RHI shader or declaration creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = RenderTargetLayouts::MakeContactVisibilityOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration = Candidate.VertexDeclaration;
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					"ContactVisibilityPipeline", Initializer);
				if (Candidate.PipelineState == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"ContactVisibility", "pipeline",
						"Graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (Payload == nullptr || FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr) return false;

		FMatrix InverseViewProjection;
		if (!Math::TryInverse(View.ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
			return false;
		FContactVisibilityUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InverseViewProjection[Col][Row]);
				Uniform.ViewProjection[Row * 4 + Col] =
					static_cast<float>(View.ViewProjectionMatrix[Col][Row]);
			}
		const FVector3 ToLight = -Math::Normalize(LightDirection);
		Uniform.ToLightMaxDistance[0] = static_cast<float>(ToLight.x);
		Uniform.ToLightMaxDistance[1] = static_cast<float>(ToLight.y);
		Uniform.ToLightMaxDistance[2] = static_cast<float>(ToLight.z);
		Uniform.ThicknessStepsOffsetReversedZ[3] =
			View.DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f;
		Uniform.Viewport[0] = 1.0f / static_cast<float>(View.ViewportWidth);
		Uniform.Viewport[1] = 1.0f / static_cast<float>(View.ViewportHeight);
		Uniform.Viewport[2] = static_cast<float>(View.ViewportX);
		Uniform.Viewport[3] = static_cast<float>(View.ViewportY);

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = RenderTargetLayouts::MakeContactVisibilityOutput();
		PassInfo.ColorRenderTargets[0] = Targets.Visibility;
		PassInfo.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "ContactVisibilityRenderPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth),
			static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
		CommandList.SetScissor(static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY), static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr || UniformBuffer.Size != sizeof(Uniform))
		{
			CommandList.EndRenderPass();
			return false;
		}
		FContactVisibilityFragmentShader::FParameters Parameters;
		Parameters.GBufferMaterial = Material;
		Parameters.GBufferNormals = Normals;
		Parameters.GBufferSurface = Surface;
		Parameters.GBufferEmissive = Emissive;
		Parameters.SceneDepth = SceneDepth;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->FragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FContactShadowVisibilityRenderer::GetRetainedTargetBytes_RenderThread() const
		-> uint64
	{
		check(IsInRenderingThread());
		return State->TargetsBySize.GetRetainedPayloadWeight(
			[](uint64 Key, const FTargets&) {
				return CalculateTargetBytes(static_cast<uint32>(Key >> 32),
					static_cast<uint32>(Key));
			});
	}

	auto FContactShadowVisibilityRenderer::ReleaseResources_RenderThread() -> void
	{
		State->TargetsBySize.Reset();
		State->Resources.Reset();
	}
} // namespace Durin
