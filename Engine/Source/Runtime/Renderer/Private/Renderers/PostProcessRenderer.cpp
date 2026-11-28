#include "Shader/MaterialShader.h"
#include "Renderers/PostProcessRenderer.h"

#include "Renderers/DisplayMapping.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MaterialSceneTextures.h"
#include "Renderers/SceneViewPreparation.h"
#include "Materials/MaterialShaderMap.h"
#include "Resources/DefaultTextureResources.h"
#include "Scene.h"
#include "SceneView.h"
#include "DynamicRHI.h"
#include "RDG/RDG.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Math/Operations.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class FPostProcessVertexShader : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(
			FPostProcessVertexShader,
			FGlobalShader,
			"/Engine/PostProcess",
			EShaderFrequency::Vertex,
			"VertexMain");
	};

	class FPostProcessInputParameters
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FPostProcessInputParameters)
			DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
			DURIN_SHADER_PARAMETER_TEXTURE(Bloom);
			DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
		DURIN_END_SHADER_PARAMETERS();
	};

	class FCopySceneColorFragmentShader : public FGlobalShader
	{
	public:
		using FParameters = FPostProcessInputParameters::FParameters;
		DURIN_INCLUDE_SHADER_PARAMETERS_FOR(FCopySceneColorFragmentShader, FPostProcessInputParameters);

		DURIN_DECLARE_GLOBAL_SHADER(
			FCopySceneColorFragmentShader,
			FGlobalShader,
			"/Engine/PostProcess",
			EShaderFrequency::Fragment,
			"CopyFragmentMain");
	};

	class FFXAAFragmentShader : public FGlobalShader
	{
	public:
		using FParameters = FPostProcessInputParameters::FParameters;
		DURIN_INCLUDE_SHADER_PARAMETERS_FOR(FFXAAFragmentShader, FPostProcessInputParameters);

		DURIN_DECLARE_GLOBAL_SHADER(
			FFXAAFragmentShader,
			FGlobalShader,
			"/Engine/PostProcess",
			EShaderFrequency::Fragment,
			"FXAAFragmentMain");
	};

	class FBloomFragmentShader : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FBloomFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(BloomSource);
			DURIN_SHADER_PARAMETER_TEXTURE(BloomDetail);
			DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(BloomPass);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FBloomFragmentShader, FGlobalShader,
			"/Engine/PostProcess", EShaderFrequency::Fragment, "BloomFragmentMain");
	};

	DURIN_IMPLEMENT_GLOBAL_SHADER(FPostProcessVertexShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FCopySceneColorFragmentShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FFXAAFragmentShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FBloomFragmentShader);

	class FWaterSnapshotFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FWaterSnapshotFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterSnapshotDepth);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FWaterSnapshotFragmentShader, FGlobalShader, "/Engine/PostProcess",
			EShaderFrequency::Fragment, "WaterSnapshotFragmentMain");
	};
	DURIN_IMPLEMENT_GLOBAL_SHADER(FWaterSnapshotFragmentShader);

	class FWaterReflectionFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FWaterReflectionFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionCoverage);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionDepth);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionNormalRoughness);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionSpecularF0);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionEnvironment);
			DURIN_SHADER_PARAMETER_TEXTURE(WaterReflectionBrdfLut);
			DURIN_SHADER_PARAMETER_SAMPLER(WaterReflectionSampler);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(WaterReflectionView);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(WaterReflectionLighting);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FWaterReflectionFragmentShader, FGlobalShader, "/Engine/PostProcess",
			EShaderFrequency::Fragment, "WaterReflectionFragmentMain");
	};
	DURIN_IMPLEMENT_GLOBAL_SHADER(FWaterReflectionFragmentShader);

	namespace
	{
		class FPostProcessMaterialShader final : public FMaterialShader
		{
		public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::PostProcess, .Domain = EMaterialDomain::PostProcess}; }
			DURIN_BEGIN_SHADER_PARAMETERS(FPostProcessMaterialShader)
				DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBuffer, true, Draw);
				DURIN_MATERIAL_PARAMETER(View, MaterialView, ConstantBuffer<MaterialViewUniform>, UniformBuffer, UniformBuffer, true, Pass);
#include "Material/SceneTexturesParameters.inl"
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MATERIAL_SHADER(FPostProcessMaterialShader, FMaterialShader,
				"/Engine/PostProcess", EShaderFrequency::Fragment, "PostProcessMaterialMain");
		};
		DURIN_IMPLEMENT_MATERIAL_SHADER(FPostProcessMaterialShader);
		const FGlobalShaderSetRegistration GPostProcessCopyShaderSet(
			"Renderer", "PostProcess.Copy",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(),
			 &FCopySceneColorFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GPostProcessFxaaShaderSet(
			"Renderer", "PostProcess.FXAA",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(),
			 &FFXAAFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GPostProcessBloomShaderSet(
			"Renderer", "PostProcess.Bloom",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(), &FBloomFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GWaterSnapshotShaderSet("Renderer", "Water.Snapshot",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(), &FWaterSnapshotFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GWaterReflectionShaderSet("Renderer", "Water.SkyReflection",
			EShaderRequestEligibility::GameAndEditor,
			{&FPostProcessVertexShader::StaticType(), &FWaterReflectionFragmentShader::StaticType()});

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			float ExposureScale = 1.0f;
			float BloomIntensity = 0.0f;
			FVector4f BloomRect{};
		};
		static_assert(sizeof(FPostProcessViewUniform) == 32);
		static_assert(alignof(FPostProcessViewUniform) == alignof(float));

		struct FBloomPassUniform
		{
			FVector4f SourceRect;
			FVector2f InvSourceSize;
			float ExposureScale;
			uint32 Stage;
		};
		static_assert(sizeof(FBloomPassUniform) == 32);

		auto GetViewRectUV(const FSceneView& View, const FVector2f& InvSize) -> FVector4f
		{
			return {View.ViewportX * InvSize.x, View.ViewportY * InvSize.y,
				View.ViewportWidth * InvSize.x, View.ViewportHeight * InvSize.y};
		}

		auto HasPostProcessDiagnostic(const FSceneView& View, const FSceneViewRenderOptions& Options) -> bool
		{
			return Options.GBufferDebugMode != EGBufferDebugMode::Disabled
				|| Options.DeferredDirectionalDebugMode != EDeferredDirectionalDebugMode::Disabled
				|| Options.GroundTruthAmbientOcclusionDebugMode != EGroundTruthAmbientOcclusionDebugMode::Disabled
				|| View.Settings.DirectionalShadow.bShowContactDebug
				|| View.Settings.DirectionalShadow.DiagnosticMode != EDirectionalShadowDiagnosticMode::Lit
				|| View.Settings.VolumetricCloud.DebugMode != EVolumetricCloudDebugMode::Lit;
		}

		auto CreatePostProcessPipeline(
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FVertexDeclarationRHIRef& VertexDeclaration,
			const FPipelineLayoutDesc& PipelineLayout,
			const FRHIRenderTargetLayout& RenderTargetLayout)
			-> FGraphicsPipelineStateInitializer
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			Initializer.RasterizerState = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
			Initializer.PipelineLayout = PipelineLayout;
			return Initializer;
		}
	} // namespace

	struct FPostProcessRenderer::FState
	{
		FSamplerRHIRef SceneColorSampler;
		struct FMaterialPipeline { FShaderRHIRef Vertex, Fragment; FVertexDeclarationRHIRef PipelineVertexDeclaration; FGraphicsPipelineStateInitializer PipelineInitializer; };
		using FMaterialSlot = TRenderResourceCreationSlot<FMaterialPipeline>;
		struct FMaterialEntry
		{
			std::shared_ptr<const FMaterialProgram> Program;
			FShaderRHIRef Vertex;
			FMaterialSlot Slot{ERenderResourceGenerationDependency::Device};
		};
		std::vector<std::unique_ptr<FMaterialEntry>> MaterialSlots;
		std::unique_ptr<RendererPrivate::FSurfaceMaterialResources> MaterialResources;
		FTextureRHIRef MaterialOutput;
		std::string MaterialDiagnostic;
	};

	FPostProcessRenderer::FPostProcessRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FPostProcessRenderer::~FPostProcessRenderer() = default;

	auto FPostProcessRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList, bool bEnableFXAA, const FRHIRenderTargetLayout& Targets, FPreparedPostProcess& OutDraw)
		-> bool
	{
		if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList)) return false;
		if (!State->SceneColorSampler)
			State->SceneColorSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
		if (!State->SceneColorSampler) return false;

		const auto& Map = GetGlobalShaderMap();
		FPreparedPostProcess Draw;
		Draw.VertexShader = Map.GetShader<FPostProcessVertexShader>();
		if (bEnableFXAA) Draw.FXAAFragmentShader = Map.GetShader<FFXAAFragmentShader>();
		else Draw.CopyFragmentShader = Map.GetShader<FCopySceneColorFragmentShader>();
		auto* Vertex = Draw.VertexShader.GetRHIShader(false);
		auto* Fragment = bEnableFXAA ? Draw.FXAAFragmentShader.GetRHIShader(false) : Draw.CopyFragmentShader.GetRHIShader(false);
		if (!Vertex || !Fragment) return false;

		Draw.SceneColorSampler = State->SceneColorSampler;
		Draw.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		Draw.PipelineInitializer = CreatePostProcessPipeline(Vertex, Fragment, Draw.VertexDeclaration,
			bEnableFXAA ? Draw.FXAAFragmentShader.GetPipelineLayout() : Draw.CopyFragmentShader.GetPipelineLayout(), Targets);
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Draw.PipelineInitializer);
			if (!Request) return false;
			FRenderPipelinePreparationBatch::Add(Request);
			if (Request->GetState() == ERHIPipelineState::Failed || Request->GetState() == ERHIPipelineState::Canceled) return false;
		}
		OutDraw = std::move(Draw);
		return true;
	}

	auto FPostProcessRenderer::PrepareBloom_RenderThread(const FSceneView& View,
		const FSceneViewRenderOptions& Options, FPreparedPostProcess& Draw) -> void
	{
		if (!View.Settings.PostProcess.bEnableBloom || HasPostProcessDiagnostic(View, Options))
			return;

		auto Shader = GetGlobalShaderMap().GetShader<FBloomFragmentShader>();
		auto* Fragment = Shader.GetRHIShader(false);
		if (!Fragment) return;
		FRHIRenderTargetLayout Targets;
		Targets.NumColorRenderTargets = 1;
		Targets.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(
			EPixelFormat::RGBA16_FLOAT, ERHIAccess::ColorAttachmentReadWrite,
			ERHIRenderTargetLoadAction::Clear);
		auto Pipeline = CreatePostProcessPipeline(Draw.VertexShader.GetRHIShader(false), Fragment,
			Draw.VertexDeclaration, Shader.GetPipelineLayout(), Targets);
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Pipeline);
			if (!Request || Request->GetState() == ERHIPipelineState::Failed
				|| Request->GetState() == ERHIPipelineState::Canceled) return;
			FRenderPipelinePreparationBatch::Add(Request);
		}
		Draw.Bloom = std::make_shared<FPreparedBloom>(FPreparedBloom{
			std::move(Shader), std::move(Pipeline), Draw.SceneColorSampler});
	}

	auto FPostProcessRenderer::DrawBloom_RenderThread(FRHICommandListImmediate& Commands,
		const FPreparedBloom& Draw, EBloomStage Stage, FRHITexture* Source,
		FRHITexture* Detail, FRHITexture* Output, const FSceneView& View) -> void
	{
		const float Width = static_cast<float>(Output->GetSizeX());
		const float Height = static_cast<float>(Output->GetSizeY());
		SetGraphicsPipelineState(Commands, Draw.PipelineInitializer);
		Commands.SetViewport(0, 0, 0, Width, Height, 1);
		Commands.SetScissor(0, 0, Width, Height);
		Commands.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		Commands.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);

		const FVector2f InvSourceSize(1.0f / Source->GetSizeX(), 1.0f / Source->GetSizeY());
		const FBloomPassUniform Uniform{
			IsBloomExtractStage(Stage) ? GetViewRectUV(View, InvSourceSize) : FVector4f{0, 0, 1, 1},
			InvSourceSize, DisplayMapping::CalculateExposureScale(View.Settings.PostProcess.ExposureEV),
			static_cast<uint32>(Stage)};
		FBloomFragmentShader::FParameters Parameters;
		Parameters.BloomSource = Source;
		Parameters.BloomDetail = Detail ? Detail : Source;
		Parameters.SceneColorSampler = Draw.Sampler;
		Parameters.BloomPass = Commands.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
		SetShaderParameters(Commands, Draw.FragmentShader, Parameters);
		Commands.DrawIndexed(3, 0, 0);
	}

	auto FPostProcessRenderer::PrepareWaterSnapshot_RenderThread(FRHICommandListImmediate& Commands,
		FPreparedWaterSnapshot& Out) -> bool
	{
		if (!FullscreenGeometry.EnsureResources_RenderThread(Commands)) return false;
		const auto& Map = GetGlobalShaderMap();
		Out.VertexShader = Map.GetShader<FPostProcessVertexShader>();
		Out.FragmentShader = Map.GetShader<FWaterSnapshotFragmentShader>();
		auto* Vertex = Out.VertexShader.GetRHIShader(false);
		auto* Fragment = Out.FragmentShader.GetRHIShader(false);
		if (!Vertex || !Fragment) return false;
		FRHIRenderTargetLayout Targets;
		Targets.NumColorRenderTargets = 2;
		Targets.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,
			ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
		Targets.ColorAttachments[1].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::R32_FLOAT,
			ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
		Out.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		Out.Pipeline = CreatePostProcessPipeline(Vertex, Fragment, Out.VertexDeclaration,
			Out.FragmentShader.GetPipelineLayout(), Targets);
		if (!FRenderPipelinePreparationBatch::ShouldPrecachePipelines()) return true;
		auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Out.Pipeline);
		if (!Request) return false;
		FRenderPipelinePreparationBatch::Add(Request);
		return Request->GetState() != ERHIPipelineState::Failed && Request->GetState() != ERHIPipelineState::Canceled;
	}

	auto FPostProcessRenderer::DrawWaterSnapshot_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
		const FPreparedWaterSnapshot& Draw, FRHITexture* Color, FRHITexture* Depth) -> void
	{
		SetGraphicsPipelineState(Commands, Draw.Pipeline);
		Commands.SetViewport(float(View.ViewportX), float(View.ViewportY), 0,
			float(View.ViewportX + View.ViewportWidth), float(View.ViewportY + View.ViewportHeight), 1);
		Commands.SetScissor(float(View.ViewportX), float(View.ViewportY), float(View.ViewportWidth), float(View.ViewportHeight));
		Commands.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		Commands.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FWaterSnapshotFragmentShader::FParameters Parameters;
		Parameters.SceneColor = Color;
		Parameters.WaterSnapshotDepth = Depth;
		SetShaderParameters(Commands, Draw.FragmentShader, Parameters);
		Commands.DrawIndexed(3, 0, 0);
	}

	auto FPostProcessRenderer::PrepareWaterReflection_RenderThread(FPreparedWaterReflection& Out) -> bool
	{
		const auto& Map = GetGlobalShaderMap();
		Out.VertexShader = Map.GetShader<FPostProcessVertexShader>();
		Out.FragmentShader = Map.GetShader<FWaterReflectionFragmentShader>();
		auto* Vertex = Out.VertexShader.GetRHIShader(false);
		auto* Fragment = Out.FragmentShader.GetRHIShader(false);
		if (!Vertex || !Fragment) return false;
		Out.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		Out.Pipeline = CreatePostProcessPipeline(Vertex, Fragment, Out.VertexDeclaration,
			Out.FragmentShader.GetPipelineLayout(), RenderTargetLayouts::MakeGraphHybridDeferredOutput());
		Out.Pipeline.BlendState = RHICreateBlendState({FRHIColorBlendState{
			.bEnable = true, .SrcColorFactor = ERHIBlendFactor::One, .DstColorFactor = ERHIBlendFactor::One,
			.ColorWriteMask = ERHIColorWriteMask::Red | ERHIColorWriteMask::Green | ERHIColorWriteMask::Blue}});
		if (!FRenderPipelinePreparationBatch::ShouldPrecachePipelines()) return true;
		auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Out.Pipeline);
		if (!Request) return false;
		FRenderPipelinePreparationBatch::Add(Request);
		return Request->GetState() != ERHIPipelineState::Failed && Request->GetState() != ERHIPipelineState::Canceled;
	}

	auto FPostProcessRenderer::DrawWaterReflection_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
		const FPreparedWaterReflection& Draw, const FWaterReflectionInputs& Inputs) -> bool
	{
		FMatrix ViewToWorld;
		if (!Math::TryInverse(View.GetTranslatedWorldToView(), ViewToWorld, 1.0e-12)) return false;
		struct alignas(16) FViewUniform
		{
			FMatrix4f ViewToWorld;
			std::array<FVector4f, 4> Projection;
			FVector4f DepthParameters;
			FVector4f Viewport;
		};
		const auto Projection = Math::TransposeToFloat(View.ProjectionMatrix);
		const FViewUniform Data{Math::TransposeToFloat(ViewToWorld),
			{Projection[0], Projection[1], Projection[2], Projection[3]},
			{View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.f : 1.f, 0.f, 0.f, 0.f},
			{float(View.ViewportX), float(View.ViewportY), float(View.ViewportWidth), float(View.ViewportHeight)}};
		const auto Uniform = Commands.CreateUniformBufferRange(&Data, sizeof(Data));
		if (!Uniform.Buffer) return false;
		SetGraphicsPipelineState(Commands, Draw.Pipeline);
		Commands.SetViewport(float(View.ViewportX), float(View.ViewportY), 0,
			float(View.ViewportX + View.ViewportWidth), float(View.ViewportY + View.ViewportHeight), 1);
		Commands.SetScissor(float(View.ViewportX), float(View.ViewportY), float(View.ViewportWidth), float(View.ViewportHeight));
		Commands.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		Commands.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FWaterReflectionFragmentShader::FParameters Parameters;
		Parameters.WaterReflectionCoverage = Inputs.Coverage;
		Parameters.WaterReflectionDepth = Inputs.Depth;
		Parameters.WaterReflectionNormalRoughness = Inputs.NormalRoughness;
		Parameters.WaterReflectionSpecularF0 = Inputs.SpecularF0;
		Parameters.WaterReflectionEnvironment = Inputs.Environment;
		Parameters.WaterReflectionBrdfLut = Inputs.BrdfLut;
		Parameters.WaterReflectionSampler = Inputs.Sampler;
		Parameters.WaterReflectionView = Uniform;
		Parameters.WaterReflectionLighting = Inputs.Lighting;
		SetShaderParameters(Commands, Draw.FragmentShader, Parameters);
		Commands.DrawIndexed(3, 0, 0);
		return true;
	}

	auto FPostProcessRenderer::DescribeSceneTargets(uint32 Width, uint32 Height)
		-> std::array<FRHITextureCreateDesc, 2>
	{
		return {
			FRHITextureCreateDesc::Create2D("SceneColor", Width, Height,
				EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy)
				.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
			FRHITextureCreateDesc::Create2D("SceneDepth", Width, Height,
				EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource)
				.SetClearValue(FClearValueBinding(0.0f, 0u))};
	}
	auto FPostProcessRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor, FRHITexture* Output,
		const FPreparedPostProcess& Draw, const FClearValueBinding& ClearColor,
		bool bPresentOutput, const FSceneView& View, FRHITexture* Bloom) -> void
	{
		if (FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return;
		}

		const uint32 Width = Output->GetSizeX();
		const uint32 Height = Output->GetSizeY();
		FRHIRenderPassInfo Pass{};
		Pass.RenderTargetLayout = Draw.PipelineInitializer.RenderTargetLayout;
		Pass.ColorRenderTargets[0] = Output;
		Pass.ColorClearValues[0] = ClearColor;
		CommandList.BeginRenderPass(Pass, bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass");
		SetGraphicsPipelineState(CommandList, Draw.PipelineInitializer);
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

		FPostProcessViewUniform ViewUniform;
		ViewUniform.InvRenderTargetSize = FVector2f(
			1.0f / static_cast<float>(Width),
			1.0f / static_cast<float>(Height));
		ViewUniform.ExposureScale =
			DisplayMapping::CalculateExposureScale(View.Settings.PostProcess.ExposureEV);
		if (Bloom)
		{
			ViewUniform.BloomIntensity = 0.15f;
			ViewUniform.BloomRect = GetViewRectUV(View, ViewUniform.InvRenderTargetSize);
		}
		const FRHIUniformBufferRange ViewUniformBuffer =
			CommandList.CreateUniformBufferRange(
				&ViewUniform,
				sizeof(ViewUniform));

		FPostProcessInputParameters::FParameters Parameters;
		Parameters.SceneColor = SceneColor;
		Parameters.Bloom = Bloom ? Bloom : SceneColor;
		Parameters.SceneColorSampler = Draw.SceneColorSampler;
		Parameters.View = ViewUniformBuffer;
		if (Draw.FXAAFragmentShader) SetShaderParameters(CommandList, Draw.FXAAFragmentShader, Parameters);
		else SetShaderParameters(CommandList, Draw.CopyFragmentShader, Parameters);

		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
	}

	auto FPostProcessRenderer::PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands,
		const FSceneView& View, const FSceneViewRenderOptions& Options, FScene* Scene, uint32 Width, uint32 Height,
		bool bRejectPreparation, bool bRejectBindings)
		-> std::shared_ptr<const FPreparedPostProcessMaterial>
	{
		using namespace RendererPrivate;
		const auto& Proxy = View.Settings.PostProcess.Material;
		if (!Proxy || HasPostProcessDiagnostic(View, Options))
		{
			State->MaterialOutput = nullptr;
			return {};
		}
		if (bRejectPreparation) return {};
		const auto* Data = &Proxy->QueryMaterial_RenderThread();
		if (!Data->CompiledProgram)
		{
			const std::string Diagnostic = Data->bPending
				? "pending without an accepted program" : "missing or failed accepted program";
			if (State->MaterialDiagnostic != Diagnostic)
				DURIN_WARN("PostProcess material bypassed: {}. Material compiler diagnostics remain on the material.", Diagnostic);
			State->MaterialDiagnostic = Diagnostic;
			State->MaterialOutput = nullptr;
			return {};
		}
		State->MaterialDiagnostic.clear();
		const auto Program = Data->CompiledProgram;
		const auto& Map = Program->GetShaderMap();
		if (Map.Domain != EMaterialDomain::PostProcess || Map.Entries.size() != 1
			|| Map.CompiledShaders.size() != 1 || Map.Entries[0].Mesh
			|| Map.Entries[0].Shader.ShaderType != "FPostProcessMaterialShader"
			|| Map.PassContractVersion != CurrentMaterialPassContractVersion) return {};
		auto* Vertex = GetGlobalShaderMap().GetShader<FPostProcessVertexShader>().GetRHIShader(false);
		if (!Vertex) return {};
		auto Found = std::ranges::find_if(State->MaterialSlots, [&](const auto& Item) {
			return Item->Program == Program && Item->Vertex.GetReference() == Vertex;
		});
		if (Found == State->MaterialSlots.end())
		{
			if (State->MaterialSlots.size() == 8) State->MaterialSlots.erase(State->MaterialSlots.begin());
			auto Entry = std::make_unique<FState::FMaterialEntry>();
			Entry->Program = Program;
			Entry->Vertex = Vertex;
			State->MaterialSlots.push_back(std::move(Entry));
			Found = std::prev(State->MaterialSlots.end());
		}
		using FResult = TRenderResourceCreateResult<FState::FMaterialPipeline>;
		const auto* Pipeline = (*Found)->Slot.Resolve(Coordinator.GetGeneration_RenderThread(), [&]() -> FResult {
			FState::FMaterialPipeline Candidate;
			Candidate.Vertex = Vertex;
			const auto& Code = Map.CompiledShaders[0];
			auto Desc = FRHIShaderCreateDesc::Create("FPostProcessMaterialShader", Code.Frequency, Code.Code, Code.CodeHash);
			Desc.SetEntryPoint(Code.BinaryEntryPoint.c_str());
			Candidate.Fragment = GDynamicRHI->RHICreateShader(Desc);
			FPipelineLayoutDesc Layout;
			if (Candidate.Vertex && Candidate.Fragment && BuildPipelineLayoutFromShaders(std::span(&Code, 1), Layout))
			{
				FRHIRenderTargetLayout Targets;
				Targets.NumColorRenderTargets = 1;
				Targets.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,
					ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
				Candidate.PipelineInitializer = CreatePostProcessPipeline(Candidate.Vertex, Candidate.Fragment,
					Candidate.PipelineVertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread(), Layout, Targets);
			}
			else return FResult::Failure(MakeRendererResourceCreateError(
				ERenderResourceCreateErrorCategory::GraphicsPipeline, "FPostProcessMaterialShader", "optional-effect",
				ERenderResourceCreateErrorReason::PipelinePreparationRejected,
				ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
			return FResult::Success(std::move(Candidate));
		}, ReportRendererResourceCreateDiagnostic);
		if (!Pipeline) return {};
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Pipeline->PipelineInitializer);
			if (!Request) return {};
			FRenderPipelinePreparationBatch::Add(Request);
			if (Request->GetState() == ERHIPipelineState::Failed || Request->GetState() == ERHIPipelineState::Canceled) return {};
		}
		auto Prepared = std::make_shared<FPreparedPostProcessMaterial>();
		if (!IsPreparedMaterialReady(*Data, "FPostProcessMaterialShader")) return {};
		Prepared->Material = *Data;
		Prepared->bRejectBindings = bRejectBindings;
		Prepared->PipelineVertexDeclaration = Pipeline->PipelineVertexDeclaration;
		Prepared->Vertex = Pipeline->Vertex; Prepared->Fragment = Pipeline->Fragment; Prepared->PipelineInitializer = Pipeline->PipelineInitializer;
		if (!State->MaterialResources) State->MaterialResources = std::make_unique<FSurfaceMaterialResources>(Coordinator, GetDefaultTextureResources());
		FResolvedSurfaceMaterial Material;
		if (!State->MaterialResources->Ensure_RenderThread(Prepared->Material, ESurfaceMaterialPass::PostProcess)
			|| !State->MaterialResources->Resolve_RenderThread(Prepared->Material, ESurfaceMaterialPass::PostProcess, {}, nullptr, nullptr, Material)) return {};
		const auto Layout = FCompiledSurfaceBindingLayout(Map.Entries[0].Bindings).WithoutSceneInputs();
		if (!Layout.IsValid()) return {};

		const auto RetainUniform = [&](FByteView Payload) {
			const auto Range = Payload.empty() ? FRHIUniformBufferRange{} : Commands.CreateUniformBufferRange(Payload.data(), Payload.size());
			if (Range.Buffer) Prepared->Resources.emplace_back(Range.Buffer);
			return Range;
		};
		const auto MaterialBuffer = RetainUniform(Prepared->Material.Representation.GetUniformPayload());
		const auto ViewBuffer = PrepareMeshViewUniform(Commands, View, false);
		if (!ViewBuffer.Buffer) return {};
		Prepared->Resources.emplace_back(ViewBuffer.Buffer);
		std::vector<FRHIUniformBufferRange> Collections;
		for (const auto& Required : Program->ActiveCollections)
		{
			const auto Snapshot = Scene ? Scene->GetMaterialParameterCollection_RenderThread(Required.CollectionId) : nullptr;
			if (Snapshot && !Snapshot->Layout.HasCompatibleSchema(Required)) return {};
			const FByteView Payload = Snapshot ? FByteView(Snapshot->Payload) : FByteView(Required.DefaultPayload);
			if (Payload.size() != Required.UniformLayout.UniformPayloadSize) return {};
			Collections.push_back(RetainUniform(Payload));
			if (!Collections.back().Buffer) return {};
		}
		FMeshDrawShaderBindings Bindings;
		if (!PrepareCompiledSurfaceMaterial(Prepared->Fragment, Layout, Material, MaterialBuffer, {}, {}, ViewBuffer, {}, Collections, Bindings)) return {};
		Prepared->Bindings = std::move(Bindings.Parameters);
		for (auto& Binding : Prepared->Bindings)
		{
			if (Binding.Resource) Prepared->Resources.emplace_back(Binding.Resource);
		}
		if (!State->MaterialOutput || State->MaterialOutput->GetSizeX() != Width || State->MaterialOutput->GetSizeY() != Height)
		{
			State->MaterialOutput = GDynamicRHI->RHICreateTexture(Commands, FRHITextureCreateDesc::Create2D(
				"PostProcessMaterialOutput", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
			if (State->MaterialOutput) Commands.TransitionTextures(std::array{FRHITextureTransition{State->MaterialOutput,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead}});
		}
		if (!State->MaterialOutput) return {};
		Prepared->Output = State->MaterialOutput;
		return Prepared;
	}

	auto FPostProcessRenderer::DrawMaterial_RenderThread(FRHICommandListImmediate& Commands,
		const FPreparedPostProcessMaterial& Prepared, FRHITexture* Input0, FRHITexture* Depth,
		const FMaterialSceneTextureUniform& Uniform) -> bool
	{
		if (Prepared.bRejectBindings || !Input0 || Input0 == Prepared.Output.GetReference() || !Prepared.Output) return false;
		auto Scene = ResolveMaterialSceneTextureBindings(Commands, std::span<const FMaterialShaderBinding>(Prepared.Material.CompiledProgram->GetShaderMap().Entries[0].Bindings), Input0, Depth, Uniform);
		if (!Scene) return false;
		auto Bindings = Prepared.Bindings;
		Bindings.insert(Bindings.end(), Scene->Parameters.begin(), Scene->Parameters.end());
		FRHIRenderPassInfo Pass;
		Pass.RenderTargetLayout.NumColorRenderTargets = 1;
		Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,
			ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
		Pass.ColorRenderTargets[0] = Prepared.Output;
		Commands.BeginRenderPass(Pass, "Scene.PostProcessMaterial");
		SetGraphicsPipelineState(Commands, Prepared.PipelineInitializer);
		const auto Width = Prepared.Output->GetSizeX(), Height = Prepared.Output->GetSizeY();
		Commands.SetViewport(0, 0, 0, Width, Height, 1); Commands.SetScissor(0, 0, Width, Height);
		Commands.SetShaderParameters(Prepared.Fragment, Bindings);
		Commands.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		Commands.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		Commands.DrawIndexed(3, 0, 0);
		Commands.EndRenderPass();
		return true;
	}

	auto FPostProcessRenderer::ReleaseResources_RenderThread() -> void
	{
		State->SceneColorSampler = nullptr;
		State->MaterialSlots.clear();
		State->MaterialOutput = nullptr;
		if (State->MaterialResources) State->MaterialResources->ReleaseResources_RenderThread();
	}
} // namespace Durin
