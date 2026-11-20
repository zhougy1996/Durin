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
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
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
		DURIN_IMPLEMENT_GLOBAL_SHADER(FPostProcessVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FCopySceneColorFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FFXAAFragmentShader);
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

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			float ExposureScale = 1.0f;
			float Padding = 0.0f;
		};
		static_assert(sizeof(FPostProcessViewUniform) == 16);
		static_assert(alignof(FPostProcessViewUniform) == alignof(float));

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

	struct FPreparedPostProcess
	{
		TShaderMapRef<FPostProcessVertexShader> VertexShader;
		TShaderMapRef<FCopySceneColorFragmentShader> CopyFragmentShader;
		TShaderMapRef<FFXAAFragmentShader> FXAAFragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer PipelineInitializer;
		FSamplerRHIRef SceneColorSampler;
	};

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
		FRHICommandListImmediate& CommandList, bool bEnableFXAA, const FRHIRenderTargetLayout& Targets)
		-> std::shared_ptr<const FPreparedPostProcess>
	{
		if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList)) return {};
		if (!State->SceneColorSampler)
			State->SceneColorSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
		if (!State->SceneColorSampler) return {};

		const auto& Map = GetGlobalShaderMap();
		auto Draw = std::make_shared<FPreparedPostProcess>();
		Draw->VertexShader = Map.GetShader<FPostProcessVertexShader>();
		if (bEnableFXAA) Draw->FXAAFragmentShader = Map.GetShader<FFXAAFragmentShader>();
		else Draw->CopyFragmentShader = Map.GetShader<FCopySceneColorFragmentShader>();
		auto* Vertex = Draw->VertexShader.GetRHIShader(false);
		auto* Fragment = bEnableFXAA ? Draw->FXAAFragmentShader.GetRHIShader(false) : Draw->CopyFragmentShader.GetRHIShader(false);
		if (!Vertex || !Fragment) return {};

		Draw->SceneColorSampler = State->SceneColorSampler;
		Draw->VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
		Draw->PipelineInitializer = CreatePostProcessPipeline(Vertex, Fragment, Draw->VertexDeclaration,
			bEnableFXAA ? Draw->FXAAFragmentShader.GetPipelineLayout() : Draw->CopyFragmentShader.GetPipelineLayout(), Targets);
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Draw->PipelineInitializer);
			if (!Request) return {};
			FRenderPipelinePreparationBatch::Add(Request);
			if (Request->GetState() == ERHIPipelineState::Failed || Request->GetState() == ERHIPipelineState::Canceled) return {};
		}
		return Draw;
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
		bool bPresentOutput, float ExposureEV) -> void
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
			DisplayMapping::CalculateExposureScale(ExposureEV);
		const FRHIUniformBufferRange ViewUniformBuffer =
			CommandList.CreateUniformBufferRange(
				&ViewUniform,
				sizeof(ViewUniform));

		FPostProcessInputParameters::FParameters Parameters;
		Parameters.SceneColor = SceneColor;
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
		if (!Proxy || Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			|| Options.DeferredDirectionalDebugMode != EDeferredDirectionalDebugMode::Disabled
			|| Options.GroundTruthAmbientOcclusionDebugMode != EGroundTruthAmbientOcclusionDebugMode::Disabled
			|| View.Settings.DirectionalShadow.bShowContactDebug
			|| View.Settings.DirectionalShadow.DiagnosticMode != EDirectionalShadowDiagnosticMode::Lit
			|| View.Settings.VolumetricCloud.DebugMode != EVolumetricCloudDebugMode::Lit)
		{
			State->MaterialOutput = nullptr;
			return {};
		}
		if (bRejectPreparation) return {};
		const auto* Data = Proxy->GetMaterialNoFallback();
		if (!Data || !Data->CompiledProgram)
		{
			const std::string Diagnostic = Proxy->IsCompiling() ? "pending without an accepted program" : "missing or failed accepted program";
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
		Prepared->Program = Program;
		Prepared->bRejectBindings = bRejectBindings;
		Prepared->PipelineVertexDeclaration = Pipeline->PipelineVertexDeclaration;
		Prepared->Vertex = Pipeline->Vertex; Prepared->Fragment = Pipeline->Fragment; Prepared->PipelineInitializer = Pipeline->PipelineInitializer;
		if (!ResolvePreparedMaterialBinding(*Data, Prepared->Material, "FPostProcessMaterialShader")) return {};
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
		const auto MaterialBuffer = RetainUniform(Prepared->Material.CompiledUniformPayload);
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
		auto Scene = ResolveMaterialSceneTextureBindings(Commands, std::span<const FMaterialShaderBinding>(Prepared.Program->GetShaderMap().Entries[0].Bindings), Input0, Depth, Uniform);
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
