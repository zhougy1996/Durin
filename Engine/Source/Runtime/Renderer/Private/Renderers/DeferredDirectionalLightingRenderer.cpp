#include "Renderers/DeferredDirectionalLightingRenderer.h"

#include "RenderPipelineCreation.h"
#include "Resources/FullscreenGeometryResources.h"
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
		FSamplerRHIRef FallbackEnvironmentSampler;
		FSamplerRHIRef FallbackShadowSampler;
		FRasterizerStateRHIRef Rasterizer;
		FBlendStateRHIRef AdditiveBlend;

		template<typename TVertexShader, typename TPixelShader>
		auto MakePipeline(const TShaderMapRef<TVertexShader>& Vertex, const TShaderMapRef<TPixelShader>& Pixel,
			const FVertexDeclarationRHIRef& Declaration, bool bProduction, bool bAdditive,
			FGraphicsPipelineStateInitializer& Pipeline) const -> bool
		{
			Pipeline.BoundShaders.VertexShader = Vertex.GetRHIShader(false);
			Pipeline.BoundShaders.FragmentShader = Pixel.GetRHIShader(false);
			if (!Pipeline.BoundShaders.VertexShader || !Pipeline.BoundShaders.FragmentShader) return false;
			Pipeline.VertexDeclaration = Declaration;
			Pipeline.RasterizerState = Rasterizer;
			Pipeline.RenderTargetLayout = bProduction ? RenderTargetLayouts::MakeGraphHybridDeferredOutput()
				: RenderTargetLayouts::MakeDeferredDirectionalOutput();
			if (bAdditive) Pipeline.BlendState = AdditiveBlend;
			Pipeline.PipelineLayout = Pixel.GetPipelineLayout();
			return true;
		}
	};

	FDeferredDirectionalLightingRenderer::FDeferredDirectionalLightingRenderer(
		FFullscreenGeometryResources& InFullscreenGeometry
	)
		: FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FDeferredDirectionalLightingRenderer::~FDeferredDirectionalLightingRenderer() = default;

	auto FDeferredDirectionalLightingRenderer::EnsureCommonResources_RenderThread(FRHICommandListImmediate& CommandList) -> bool
	{
		if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList)) return false;
		if (!State->Rasterizer) State->Rasterizer = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
		if (!State->AdditiveBlend)
		{
			FBlendStateInitializerRHI Blend{};
			Blend[0] = {.bEnable = true, .DstColorFactor = ERHIBlendFactor::One,
				.SrcAlphaFactor = ERHIBlendFactor::Zero, .DstAlphaFactor = ERHIBlendFactor::One};
			State->AdditiveBlend = RHICreateBlendState(Blend);
		}
		if (!State->FallbackEnvironmentSampler)
			State->FallbackEnvironmentSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
		if (!State->FallbackShadowSampler)
		{
			FRHISamplerDesc ShadowDesc = FRHISamplerDesc::LinearClamp();
			ShadowDesc.AddressU = ESamplerAddressMode::ClampToBorder;
			ShadowDesc.AddressV = ESamplerAddressMode::ClampToBorder;
			ShadowDesc.AddressW = ESamplerAddressMode::ClampToBorder;
			ShadowDesc.BorderColor = ESamplerBorderColor::FloatOpaqueWhite;
			ShadowDesc.bEnableCompare = true;
			ShadowDesc.CompareOp = ESamplerCompareOp::LessOrEqual;
			State->FallbackShadowSampler = RHICreateSampler(ShadowDesc);
		}
		return State->Rasterizer && State->AdditiveBlend && State->FallbackEnvironmentSampler && State->FallbackShadowSampler;
	}

	auto FDeferredDirectionalLightingRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList, bool bProduction, bool bIsolated) -> bool
	{
		if (!EnsureCommonResources_RenderThread(CommandList)) return false;
		const auto& Map = GetGlobalShaderMap();
		const auto Check = [&](const auto& Vertex, const auto& Pixel, bool bProductionPass, bool bAdditive, const char* Name) {
			if (!FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
				return Vertex.GetRHIShader(false) && Pixel.GetRHIShader(false);
			FGraphicsPipelineStateInitializer Pipeline;
			if (!State->MakePipeline(Vertex, Pixel, FullscreenGeometry.GetVertexDeclaration_RenderThread(),
				bProductionPass, bAdditive, Pipeline)) return false;
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Pipeline, Name);
			if (!Request) return false;
			FRenderPipelinePreparationBatch::Add(Request);
			const auto Status = Request->GetState();
			return Status != ERHIPipelineState::Failed && Status != ERHIPipelineState::Canceled;
		};
		if (bIsolated && !Check(Map.GetShader<FDeferredDirectionalVertexShader>(), Map.GetShader<FDeferredDirectionalFragmentShader>(),
			false, false, "DeferredDirectionalLightingPipeline")) return false;
		if (!bProduction) return true;
		const auto Vertex = Map.GetShader<FDeferredProductionVertexShader>();
		const auto Pixel = Map.GetShader<FDeferredProductionFragmentShader>();
		if (!Check(Vertex, Pixel, true, true, "DeferredProductionLightingPipeline")) return false;
		return !FRenderPipelinePreparationBatch::ShouldPrecachePipelines()
			|| Check(Vertex, Pixel, true, false, "DeferredProductionDiagnosticPipeline");
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
		if (!EnsureCommonResources_RenderThread(CommandList)) return false;
		const auto& Map = GetGlobalShaderMap();
		const auto& Declaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		TShaderMapRef<FDeferredDirectionalFragmentShader> FragmentShader;
		TShaderMapRef<FDeferredProductionFragmentShader> ProductionFragmentShader;
		FGraphicsPipelineStateInitializer Pipeline;
		if (bProduction)
		{
			const bool bAdditive = !Parameters.bContactVisibilityDebug
				&& View->Settings.DirectionalShadow.DiagnosticMode == EDirectionalShadowDiagnosticMode::Lit;
			ProductionFragmentShader = Map.GetShader<FDeferredProductionFragmentShader>();
			if (!State->MakePipeline(Map.GetShader<FDeferredProductionVertexShader>(), ProductionFragmentShader,
				Declaration, true, bAdditive, Pipeline)) return false;
		}
		else
		{
			FragmentShader = Map.GetShader<FDeferredDirectionalFragmentShader>();
			if (!State->MakePipeline(Map.GetShader<FDeferredDirectionalVertexShader>(), FragmentShader,
				Declaration, false, false, Pipeline)) return false;
		}
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
			ShaderParameters.EnvironmentSampler = Parameters.EnvironmentSampler ? Parameters.EnvironmentSampler : State->FallbackEnvironmentSampler.GetReference();
			ShaderParameters.DirectionalShadowTexture =
				Parameters.DirectionalShadowTexture;
			ShaderParameters.DirectionalShadowSampler =
				Parameters.DirectionalShadowSampler ? Parameters.DirectionalShadowSampler : State->FallbackShadowSampler.GetReference();
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
			SetShaderParameters(CommandList, ProductionFragmentShader, ShaderParameters);
		}
		else
		{
			FDeferredDirectionalFragmentShader::FParameters ShaderParameters;
			FillShaderParameters(ShaderParameters);
			SetShaderParameters(
				CommandList, FragmentShader, ShaderParameters
			);
		}
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FDeferredDirectionalLightingRenderer::ReleaseResources_RenderThread()
		-> void
	{
		*State = {};
	}
} // namespace Durin
