#include "Renderers/DeferredDirectionalLightingRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
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
		class FDeferredDirectionalVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FDeferredDirectionalVertexShader, FGlobalShader, "/Engine/DeferredDirectionalLighting", EShaderFrequency::Vertex, "VertexMain");
		};

		class FDeferredProductionVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FDeferredProductionVertexShader, FGlobalShader, "/Engine/DeferredProductionLighting", EShaderFrequency::Vertex, "VertexMain");
		};

#define DURIN_DEFERRED_FRAGMENT_PARAMETERS()                             \
	DURIN_SHADER_PARAMETER_TEXTURE(GBufferMaterial);                     \
	DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);                      \
	DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);                      \
	DURIN_SHADER_PARAMETER_TEXTURE(GBufferModelData);                    \
	DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);                          \
	DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentIrradiance);               \
	DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentPrefiltered);              \
	DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentBrdfLut);                  \
	DURIN_SHADER_PARAMETER_SAMPLER(EnvironmentSampler);                  \
	DURIN_SHADER_PARAMETER_TEXTURE(DirectionalShadowTexture);            \
	DURIN_SHADER_PARAMETER_SAMPLER(DirectionalShadowSampler);            \
	DURIN_SHADER_PARAMETER_TEXTURE(GroundTruthAmbientOcclusionRaw);      \
	DURIN_SHADER_PARAMETER_TEXTURE(GroundTruthAmbientOcclusionFiltered); \
	DURIN_SHADER_PARAMETER_TEXTURE(GroundTruthAmbientOcclusionResolved); \
	DURIN_SHADER_PARAMETER_TEXTURE(GroundTruthAmbientOcclusionSelector); \
	DURIN_SHADER_PARAMETER_TEXTURE(ContactVisibility);                   \
	DURIN_SHADER_PARAMETER_TEXTURE(VolumetricCloudVisibility);           \
	DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);                 \
	DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);             \
	DURIN_SHADER_PARAMETER_STORAGE_BUFFER(LocalLights);

		class FDeferredDirectionalFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FDeferredDirectionalFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(BasePassColor);
				DURIN_DEFERRED_FRAGMENT_PARAMETERS()
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FDeferredDirectionalFragmentShader, FGlobalShader, "/Engine/DeferredDirectionalLighting", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FDeferredProductionFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FDeferredProductionFragmentShader)
				DURIN_DEFERRED_FRAGMENT_PARAMETERS()
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FDeferredProductionFragmentShader, FGlobalShader, "/Engine/DeferredProductionLighting", EShaderFrequency::Fragment, "ProductionFragmentMain");
		};

#undef DURIN_DEFERRED_FRAGMENT_PARAMETERS
		DURIN_IMPLEMENT_GLOBAL_SHADER(FDeferredDirectionalVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FDeferredProductionVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FDeferredDirectionalFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FDeferredProductionFragmentShader);
		const FGlobalShaderSetRegistration GDeferredDirectionalShaderSet(
			"Renderer", "DeferredDirectionalLighting.LocalBuffer",
			EShaderRequestEligibility::GameAndEditor,
			{&FDeferredDirectionalVertexShader::StaticType(),
			 &FDeferredDirectionalFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GDeferredProductionShaderSet(
			"Renderer", "DeferredProductionLighting.LocalBuffer",
			EShaderRequestEligibility::GameAndEditor,
			{&FDeferredProductionVertexShader::StaticType(),
			 &FDeferredProductionFragmentShader::StaticType()});
	} // namespace

	struct FDeferredDirectionalLightingRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef ShaderSet;
			FGlobalShaderSetRef ProductionShaderSet;
			TShaderMapRef<FDeferredDirectionalVertexShader> VertexShader;
			TShaderMapRef<FDeferredProductionVertexShader> ProductionVertexShader;
			TShaderMapRef<FDeferredDirectionalFragmentShader> FragmentShader;
			TShaderMapRef<FDeferredProductionFragmentShader> ProductionFragmentShader;
			FVertexDeclarationRHIRef PipelineVertexDeclaration;
			FGraphicsPipelineStateInitializer PipelineInitializer;
			FGraphicsPipelineStateInitializer ProductionPipelineInitializer;
			FGraphicsPipelineStateInitializer DiagnosticPipelineInitializer;
			FSamplerRHIRef FallbackEnvironmentSampler;
			FSamplerRHIRef FallbackShadowSampler;
		};

		TRenderResourceCreationSlot<FPayload> Resources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
	};

	FDeferredDirectionalLightingRenderer::FDeferredDirectionalLightingRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry
	)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FDeferredDirectionalLightingRenderer::~FDeferredDirectionalLightingRenderer() = default;

	auto FDeferredDirectionalLightingRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList
	) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> Types{
					&FDeferredDirectionalVertexShader::StaticType(),
					&FDeferredDirectionalFragmentShader::StaticType()};
				FPayload Candidate;
				const auto ShaderSetCompletion = GetGlobalShaderMap().ResolveShaderSet(
					"DeferredDirectionalLighting.LocalBuffer", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (ShaderSetCompletion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
				if (ShaderSetCompletion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
				if (!ShaderSetCompletion)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"DeferredDirectionalLighting", "shader",
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual
					));
				}
				const std::array<const FGlobalShaderType*, 2> ProductionTypes{
					&FDeferredProductionVertexShader::StaticType(),
					&FDeferredProductionFragmentShader::StaticType()};
				const auto ProductionCompletion = GetGlobalShaderMap().ResolveShaderSet(
					"DeferredProductionLighting.LocalBuffer", ProductionTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (ProductionCompletion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
				if (ProductionCompletion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
				if (!ProductionCompletion) return FResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::ShaderCompile, "DeferredProductionLighting", "shader",
					ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual));
				Candidate.ProductionShaderSet = *ProductionCompletion;
				Candidate.ShaderSet = *ShaderSetCompletion;
				Candidate.VertexShader = TShaderMapRef<FDeferredDirectionalVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FDeferredDirectionalFragmentShader>(Candidate.ShaderSet);
				Candidate.ProductionVertexShader = TShaderMapRef<FDeferredProductionVertexShader>(Candidate.ProductionShaderSet);
				Candidate.ProductionFragmentShader = TShaderMapRef<FDeferredProductionFragmentShader>(Candidate.ProductionShaderSet);

				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"DeferredDirectionalLighting", "fullscreen-geometry",
						ERenderResourceCreateErrorReason::FullscreenGeometryUnavailable,
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual
					));
				}
				FRHIShader* VertexRHI =
					Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI =
					Candidate.FragmentShader.GetRHIShader(false);
				FRHIShader* ProductionVertexRHI = Candidate.ProductionVertexShader.GetRHIShader(false);
				FRHIShader* ProductionFragmentRHI =
					Candidate.ProductionFragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr
					|| ProductionVertexRHI == nullptr || ProductionFragmentRHI == nullptr)
				{
					return FResult::Pending();
				}
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeDeferredDirectionalOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					Candidate.PipelineVertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineInitializer = Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGraphHybridDeferredOutput();
				Initializer.BoundShaders.FragmentShader = ProductionFragmentRHI;
				Initializer.BoundShaders.VertexShader = ProductionVertexRHI;
				Initializer.PipelineLayout = Candidate.ProductionShaderSet.GetPipelineLayout();
				Initializer.ColorBlendStates[0] = {.bEnable = true, .DstColorFactor = ERHIBlendFactor::One,
					.SrcAlphaFactor = ERHIBlendFactor::Zero, .DstAlphaFactor = ERHIBlendFactor::One};
				Candidate.ProductionPipelineInitializer = Initializer;
				Initializer.ColorBlendStates[0].bEnable = false;
				Candidate.DiagnosticPipelineInitializer = Initializer;
				Candidate.FallbackEnvironmentSampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				FRHISamplerDesc ShadowDesc = FRHISamplerDesc::LinearClamp();
				ShadowDesc.AddressU = ESamplerAddressMode::ClampToBorder;
				ShadowDesc.AddressV = ESamplerAddressMode::ClampToBorder;
				ShadowDesc.AddressW = ESamplerAddressMode::ClampToBorder;
				ShadowDesc.BorderColor = ESamplerBorderColor::FloatOpaqueWhite;
				ShadowDesc.bEnableCompare = true;
				ShadowDesc.CompareOp = ESamplerCompareOp::LessOrEqual;
				Candidate.FallbackShadowSampler = RHICreateSampler(ShadowDesc);
				if (Candidate.FallbackEnvironmentSampler == nullptr
					|| Candidate.FallbackShadowSampler == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"DeferredDirectionalLighting", "fallback samplers",
						ERenderResourceCreateErrorReason::Unspecified,
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual
					));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable
		);
		if (Payload && FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto PipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload->PipelineInitializer, "DeferredDirectionalLightingPipeline");
			if (!PipelineState) return false;
			FRenderPipelinePreparationBatch::Add(PipelineState);
			if (PipelineState->GetState() == ERHIPipelineState::Failed || PipelineState->GetState() == ERHIPipelineState::Canceled) return false;
			auto ProductionPipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload->ProductionPipelineInitializer, "DeferredProductionLightingPipeline");
			if (!ProductionPipelineState) return false;
			FRenderPipelinePreparationBatch::Add(ProductionPipelineState);
			if (ProductionPipelineState->GetState() == ERHIPipelineState::Failed || ProductionPipelineState->GetState() == ERHIPipelineState::Canceled) return false;
			auto DiagnosticPipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload->DiagnosticPipelineInitializer, "DeferredProductionDiagnosticPipeline");
			if (!DiagnosticPipelineState) return false;
			FRenderPipelinePreparationBatch::Add(DiagnosticPipelineState);
			if (DiagnosticPipelineState->GetState() == ERHIPipelineState::Failed || DiagnosticPipelineState->GetState() == ERHIPipelineState::Canceled) return false;
		}
		return Payload != nullptr;
	}

	auto FDeferredDirectionalLightingRenderer::DescribeTarget(
		uint32 Width, uint32 Height) -> FRHITextureCreateDesc
	{
		return FRHITextureCreateDesc::Create2D(
											   "DeferredDirectionalColor", Width, Height,
											   EPixelFormat::RGBA16_FLOAT
		)
											   .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
											   .SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f));
	}

	auto FDeferredDirectionalLightingRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets& Targets,
		const FRenderParameters& Parameters
	) -> bool
	{
		return RenderInternal_RenderThread(
			CommandList, Targets.Color, Parameters, false
		);
	}

	auto FDeferredDirectionalLightingRenderer::RenderProduction_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FRHIRenderPassInfo& Pass,
		const FRenderParameters& Parameters
	) -> bool
	{
		return RenderInternal_RenderThread(
			CommandList, Pass.ColorRenderTargets[0], Parameters, true, &Pass
		);
	}

	auto FDeferredDirectionalLightingRenderer::RenderInternal_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		const FRenderParameters& Parameters,
		bool bProduction,
		const FRHIRenderPassInfo* ProductionPass
	) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView* View = Parameters.View;
		if (SceneColor == nullptr || View == nullptr
			|| View->ViewportWidth == 0 || View->ViewportHeight == 0
			|| Parameters.Material == nullptr || Parameters.Normals == nullptr
			|| Parameters.Surface == nullptr || (!bProduction && Parameters.BasePassColor == nullptr)
			|| Parameters.Depth == nullptr
			|| Parameters.EnvironmentIrradiance == nullptr
			|| Parameters.EnvironmentPrefiltered == nullptr
			|| Parameters.EnvironmentBrdfLut == nullptr
			|| Parameters.DirectionalShadowTexture == nullptr
			|| Parameters.GroundTruthAmbientOcclusionRaw == nullptr
			|| Parameters.GroundTruthAmbientOcclusionFiltered == nullptr
			|| Parameters.GroundTruthAmbientOcclusionResolved == nullptr
			|| Parameters.GroundTruthAmbientOcclusionSelector == nullptr
			|| Parameters.ContactVisibility == nullptr
			|| Parameters.VolumetricCloudVisibility == nullptr
			|| Parameters.Lighting.Buffer == nullptr)
		{
			return false;
		}
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		if (Payload == nullptr)
			return false;
		const bool bReplaceDiagnostic = Parameters.bContactVisibilityDebug
			|| View->Settings.DirectionalShadow.DiagnosticMode != EDirectionalShadowDiagnosticMode::Lit;
		const FGraphicsPipelineStateInitializer& Pipeline = bProduction
			? (bReplaceDiagnostic ? Payload->DiagnosticPipelineInitializer : Payload->ProductionPipelineInitializer)
			: Payload->PipelineInitializer;
		if (FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return false;
		}

		FMatrix ViewToWorld;
		if (!Math::TryInverse(View->GetTranslatedWorldToView(), ViewToWorld, 1.0e-12))
			return false;
		FViewUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View->ProjectionMatrix[Col][Row]);
				Uniform.ViewToWorld[Row * 4 + Col] =
					static_cast<float>(ViewToWorld[Col][Row]);
			}
		}
		Uniform.ClearColor = View->ClearColor;
		Uniform.Params = {
			1.0f / static_cast<float>(View->ViewportWidth),
			1.0f / static_cast<float>(View->ViewportHeight),
			Parameters.bGroundTruthAmbientOcclusionEnabled ? static_cast<float>(Parameters.DiagnosticMode) : -static_cast<float>(Parameters.DiagnosticMode + 1u),
			bProduction ? 1.0f :
						  -static_cast<float>(Parameters.GroundTruthAmbientOcclusionDebugMode)
		};
		Uniform.ContactParams = {
			Parameters.bContactVisibilityEnabled ? 1.0f : 0.0f,
			Parameters.bContactVisibilityDebug ? 1.0f : 0.0f,
			Parameters.bGroundTruthAmbientOcclusionHalfResolution ? 1.0f : 0.0f,
			Parameters.bVolumetricCloudVisibilityEnabled ? 1.0f : 0.0f
		};
		const FRHIUniformBufferRange ViewUniform =
			CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
		if (ViewUniform.Buffer == nullptr || ViewUniform.Size != sizeof(Uniform))
			return false;

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = bProduction ? RenderTargetLayouts::MakeGraphHybridDeferredOutput() : RenderTargetLayouts::MakeDeferredDirectionalOutput();
		PassInfo.ColorRenderTargets[0] = SceneColor;
		PassInfo.ColorClearValues[0] = FClearValueBinding(
			View->ClearColor.r, View->ClearColor.g,
			View->ClearColor.b, View->ClearColor.a
		);
		if (ProductionPass) PassInfo = *ProductionPass;
		CommandList.BeginRenderPass(PassInfo, bProduction ? "DeferredProductionLightingRenderPass" : "DeferredDirectionalQualificationRenderPass");
		SetGraphicsPipelineState(CommandList, Pipeline);
		CommandList.SetViewport(
			static_cast<float>(View->ViewportX),
			static_cast<float>(View->ViewportY), 0.0f,
			static_cast<float>(View->ViewportX + View->ViewportWidth),
			static_cast<float>(View->ViewportY + View->ViewportHeight), 1.0f
		);
		CommandList.SetScissor(
			static_cast<float>(View->ViewportX),
			static_cast<float>(View->ViewportY),
			static_cast<float>(View->ViewportWidth),
			static_cast<float>(View->ViewportHeight)
		);
		CommandList.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0
		);

		auto FillShaderParameters = [&](auto& ShaderParameters) {
			ShaderParameters.GBufferMaterial = Parameters.Material;
			ShaderParameters.GBufferNormals = Parameters.Normals;
			ShaderParameters.GBufferSurface = Parameters.Surface;
			ShaderParameters.GBufferModelData = Parameters.ModelData ? Parameters.ModelData : Parameters.Material;
			if constexpr (requires { ShaderParameters.BasePassColor; })
				ShaderParameters.BasePassColor = Parameters.BasePassColor;
			ShaderParameters.SceneDepth = Parameters.Depth;
			ShaderParameters.EnvironmentIrradiance =
				Parameters.EnvironmentIrradiance;
			ShaderParameters.EnvironmentPrefiltered =
				Parameters.EnvironmentPrefiltered;
			ShaderParameters.EnvironmentBrdfLut = Parameters.EnvironmentBrdfLut;
			ShaderParameters.EnvironmentSampler = Parameters.EnvironmentSampler ? Parameters.EnvironmentSampler : Payload->FallbackEnvironmentSampler.GetReference();
			ShaderParameters.DirectionalShadowTexture =
				Parameters.DirectionalShadowTexture;
			ShaderParameters.DirectionalShadowSampler =
				Parameters.DirectionalShadowSampler ? Parameters.DirectionalShadowSampler : Payload->FallbackShadowSampler.GetReference();
			ShaderParameters.GroundTruthAmbientOcclusionRaw =
				Parameters.GroundTruthAmbientOcclusionRaw;
			ShaderParameters.GroundTruthAmbientOcclusionFiltered =
				Parameters.GroundTruthAmbientOcclusionFiltered;
			ShaderParameters.GroundTruthAmbientOcclusionResolved =
				Parameters.GroundTruthAmbientOcclusionResolved;
			ShaderParameters.GroundTruthAmbientOcclusionSelector =
				Parameters.GroundTruthAmbientOcclusionSelector;
			ShaderParameters.ContactVisibility = Parameters.ContactVisibility;
			ShaderParameters.VolumetricCloudVisibility = Parameters.VolumetricCloudVisibility;
			ShaderParameters.View = ViewUniform;
			ShaderParameters.Lighting = Parameters.Lighting;
			ShaderParameters.LocalLights = Parameters.LocalLights;
		};
		if (bProduction)
		{
			FDeferredProductionFragmentShader::FParameters ShaderParameters;
			FillShaderParameters(ShaderParameters);
			SetShaderParameters(CommandList, Payload->ProductionFragmentShader, ShaderParameters);
		}
		else
		{
			FDeferredDirectionalFragmentShader::FParameters ShaderParameters;
			FillShaderParameters(ShaderParameters);
			SetShaderParameters(
				CommandList, Payload->FragmentShader, ShaderParameters
			);
		}
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FDeferredDirectionalLightingRenderer::ReleaseResources_RenderThread()
		-> void
	{
		State->Resources.Reset();
	}
} // namespace Durin
