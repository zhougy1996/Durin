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

		class FCopySceneColorFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCopySceneColorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

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
			DURIN_BEGIN_SHADER_PARAMETERS(FFXAAFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FFXAAFragmentShader,
				FGlobalShader,
				"/Engine/PostProcess",
				EShaderFrequency::Fragment,
				"FXAAFragmentMain");
		};
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

		auto GetPostProcessVariantIndex(bool bPresentOutput, bool bEnableFXAA,
			bool bHasEditorAssistance, EPixelFormat OutputFormat) -> size_t
		{
			const size_t Output = bHasEditorAssistance ? 0 : bPresentOutput ? 2 : 1;
			return Output * 4 + (OutputFormat == EPixelFormat::SBGRA8_UNORM ? 2 : 0) + (bEnableFXAA ? 1 : 0);
		}

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			float ExposureScale = 1.0f;
			float Padding = 0.0f;
		};
		static_assert(sizeof(FPostProcessViewUniform) == 16);
		static_assert(alignof(FPostProcessViewUniform) == alignof(float));

		auto CreatePostProcessPipeline(
			FName PipelineName,
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FVertexDeclarationRHIRef& VertexDeclaration,
			const FPipelineLayoutDesc& PipelineLayout,
			const FRHIRenderTargetLayout& RenderTargetLayout)
			-> FGraphicsPipelineStateRHIRef
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			Initializer.RasterizerState.CullMode = ERHICullMode::None;
			Initializer.PipelineLayout = PipelineLayout;
			return FRenderPipelineRequestScope::Graphics(
				PipelineName,
				Initializer);
		}
	} // namespace

	struct FPostProcessRenderer::FState
	{
		struct FPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FPostProcessVertexShader> VertexShader;
			TShaderMapRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderMapRef<FFXAAFragmentShader> FXAAFragmentShader;
			FGraphicsPipelineStateRHIRef Pipeline;
			FSamplerRHIRef SceneColorSampler;
		};
		using FSlot = TRenderResourceCreationSlot<FPayload>;
		// Two shader routes, two formats, and three output attachment layouts.
		std::array<std::unique_ptr<FSlot>, 12> Slots;
		struct FMaterialPipeline { FShaderRHIRef Vertex, Fragment; FGraphicsPipelineStateRHIRef Pipeline; };
		using FMaterialSlot = TRenderResourceCreationSlot<FMaterialPipeline>;
		std::vector<std::pair<std::shared_ptr<const FMaterialProgram>, std::unique_ptr<FMaterialSlot>>> MaterialSlots;
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

	auto FPostProcessRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList, bool bPresentOutput, bool bEnableFXAA,
		bool bHasEditorAssistance, EPixelFormat OutputFormat) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		auto& Slot = State->Slots[GetPostProcessVariantIndex(bPresentOutput,
			bEnableFXAA, bHasEditorAssistance, OutputFormat)];
		if (!Slot) Slot = std::make_unique<FState::FSlot>(
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device);
		return Slot->Resolve(Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList, bPresentOutput, bEnableFXAA, bHasEditorAssistance, OutputFormat]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> ShaderTypes = {
					&FPostProcessVertexShader::StaticType(),
					bEnableFXAA ? &FFXAAFragmentShader::StaticType() : &FCopySceneColorFragmentShader::StaticType()};
				const auto Completion = GetGlobalShaderMap().ResolveShaderSet(
					bEnableFXAA ? "PostProcess.FXAA" : "PostProcess.Copy", ShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (Completion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
				if (Completion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
				if (!Completion) return FResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::ShaderCompile, "PostProcess", bEnableFXAA ? "fxaa" : "copy",
					ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual));
				FPayload Candidate;
				Candidate.ShaderSet = *Completion;
				Candidate.VertexShader = TShaderMapRef<FPostProcessVertexShader>(Candidate.ShaderSet);
				FRHIShader* FragmentRHI = nullptr;
				if (bEnableFXAA)
				{
					Candidate.FXAAFragmentShader = TShaderMapRef<FFXAAFragmentShader>(Candidate.ShaderSet);
					FragmentRHI = Candidate.FXAAFragmentShader.GetRHIShader(false);
				}
				else
				{
					Candidate.CopyFragmentShader = TShaderMapRef<FCopySceneColorFragmentShader>(Candidate.ShaderSet);
					FragmentRHI = Candidate.CopyFragmentShader.GetRHIShader(false);
				}
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource, "PostProcess", "fullscreen-geometry",
						ERenderResourceCreateErrorReason::FullscreenGeometryUnavailable,
						ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
				Candidate.SceneColorSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				if (!Candidate.SceneColorSampler || !VertexRHI || !FragmentRHI)
					return FResult::Pending();
				const bool bBGRA = OutputFormat == EPixelFormat::SBGRA8_UNORM;
				const auto Format = bBGRA ? EPixelFormat::SBGRA8_UNORM : EPixelFormat::SRGBA8_UNORM;
				const auto Layout = bHasEditorAssistance
					? RenderTargetLayouts::MakeScenePostProcessOutput(Format)
					: RenderTargetLayouts::MakeFinalScenePostProcessOutput(bPresentOutput
						? RenderTargetLayouts::EViewportOutput::Present : RenderTargetLayouts::EViewportOutput::Offscreen, Format);
				const FName Name(std::format("PostProcess{}{}{}Pipeline", bEnableFXAA ? "FXAA" : "Copy",
					bBGRA ? "BGRA" : "", bHasEditorAssistance ? "Intermediate" : bPresentOutput ? "Present" : "Offscreen"));
				Candidate.Pipeline = CreatePostProcessPipeline(Name, VertexRHI, FragmentRHI,
					FullscreenGeometry.GetVertexDeclaration_RenderThread(), Candidate.ShaderSet.GetPipelineLayout(), Layout);
				if (!Candidate.Pipeline) return FResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "PostProcess", bEnableFXAA ? "fxaa" : "copy",
					ERenderResourceCreateErrorReason::PipelinePreparationRejected,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
						| ERenderResourceGenerationDependency::Manual
				));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable) != nullptr;
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
		FRHITexture* SceneColor,
		uint32 Width,
		uint32 Height,
		bool bPresentOutput,
		bool bEnableFXAA,
		bool bHasEditorAssistance,
		EPixelFormat OutputFormat,
		float ExposureEV) -> void
	{
		const auto& Slot = State->Slots[GetPostProcessVariantIndex(bPresentOutput,
			bEnableFXAA, bHasEditorAssistance, OutputFormat)];
		const FState::FPayload* Payload = Slot ? Slot->GetPayload() : nullptr;
		if (!Payload) return;
		const FGraphicsPipelineStateRHIRef& PipelineState = Payload->Pipeline;
		if (PipelineState == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return;
		}

		CommandList.SetGraphicsPipelineState(*PipelineState);
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

		if (bEnableFXAA)
		{
			FFXAAFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SceneColor = SceneColor;
			FragmentParameters.SceneColorSampler =
				Payload->SceneColorSampler;
			FragmentParameters.View = ViewUniformBuffer;
			SetShaderParameters(
				CommandList,
				Payload->FXAAFragmentShader.GetShaderRef(),
				FragmentParameters);
		}
		else
		{
			FCopySceneColorFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SceneColor = SceneColor;
			FragmentParameters.SceneColorSampler =
				Payload->SceneColorSampler;
			FragmentParameters.View = ViewUniformBuffer;
			SetShaderParameters(
				CommandList,
				Payload->CopyFragmentShader.GetShaderRef(),
				FragmentParameters);
		}

		CommandList.DrawIndexed(3, 0, 0);
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
			|| Map.Entries[0].Shader.ShaderType != "PostProcessMaterial"
			|| Map.PassContractVersion != CurrentMaterialPassContractVersion) return {};
		auto Found = std::ranges::find_if(State->MaterialSlots, [&](const auto& Item) { return Item.first == Program; });
		if (Found == State->MaterialSlots.end())
		{
			if (State->MaterialSlots.size() == 8) State->MaterialSlots.erase(State->MaterialSlots.begin());
			State->MaterialSlots.emplace_back(Program, std::make_unique<FState::FMaterialSlot>(
				ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device));
			Found = std::prev(State->MaterialSlots.end());
		}
		using FResult = TRenderResourceCreateResult<FState::FMaterialPipeline>;
		const auto* Pipeline = Found->second->Resolve(Coordinator.GetGeneration_RenderThread(), [&]() -> FResult {
			FState::FMaterialPipeline Candidate;
			for (const auto& Slot : State->Slots)
				if (Slot && Slot->GetPayload()) { Candidate.Vertex = Slot->GetPayload()->VertexShader.GetRHIShader(false); break; }
			const auto& Code = Map.CompiledShaders[0];
			auto Desc = FRHIShaderCreateDesc::Create("PostProcessMaterial", Code.Frequency, Code.Code, Code.CodeHash);
			Desc.SetEntryPoint(Code.BinaryEntryPoint.c_str());
			Candidate.Fragment = GDynamicRHI->RHICreateShader(Desc);
			FPipelineLayoutDesc Layout;
			if (Candidate.Vertex && Candidate.Fragment && BuildPipelineLayoutFromShaders(std::span(&Code, 1), Layout))
			{
				FRHIRenderTargetLayout Targets;
				Targets.NumColorRenderTargets = 1;
				Targets.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,
					ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
				Candidate.Pipeline = CreatePostProcessPipeline("PostProcessMaterial", Candidate.Vertex, Candidate.Fragment,
					FullscreenGeometry.GetVertexDeclaration_RenderThread(), Layout, Targets);
			}
			if (!Candidate.Pipeline) return FResult::Failure(MakeRendererResourceCreateError(
				ERenderResourceCreateErrorCategory::GraphicsPipeline, "PostProcessMaterial", "optional-effect",
				ERenderResourceCreateErrorReason::PipelinePreparationRejected,
				ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
			return FResult::Success(std::move(Candidate));
		}, ReportRendererResourceCreateDiagnostic);
		if (!Pipeline) return {};
		auto Prepared = std::make_shared<FPreparedPostProcessMaterial>();
		Prepared->Program = Program;
		Prepared->bRejectBindings = bRejectBindings;
		Prepared->Vertex = Pipeline->Vertex; Prepared->Fragment = Pipeline->Fragment; Prepared->Pipeline = Pipeline->Pipeline;
		if (!ResolvePreparedMaterialBinding(*Data, Prepared->Material, "PostProcessMaterial")) return {};
		if (!State->MaterialResources) State->MaterialResources = std::make_unique<FSurfaceMaterialResources>(Coordinator, GetDefaultTextureResources());
		FResolvedSurfaceMaterial Material;
		if (!State->MaterialResources->Ensure_RenderThread(Prepared->Material, ESurfaceMaterialPass::PostProcess)
			|| !State->MaterialResources->Resolve_RenderThread(Prepared->Material, ESurfaceMaterialPass::PostProcess, {}, nullptr, nullptr, Material)) return {};
		auto Reflection = Map.CompiledShaders[0].Reflection;
		std::erase_if(Reflection.ResourceBindings, [](const auto& B) { return B.SetIndex == 0 && B.BindingIndex >= 40 && B.BindingIndex <= 44; });
		const FCompiledSurfaceBindingLayout Layout(Reflection);
		if (!Layout.IsValid()) return {};
		for (const auto& Entry : Layout.GetEntries())
			if (Entry.Source != FCompiledSurfaceBindingLayout::ESource::View && Entry.Source != FCompiledSurfaceBindingLayout::ESource::Material
				&& Entry.Source != FCompiledSurfaceBindingLayout::ESource::Texture && Entry.Source != FCompiledSurfaceBindingLayout::ESource::Sampler
				&& Entry.Source != FCompiledSurfaceBindingLayout::ESource::Collection) return {};
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
			if (Binding.Type == ERHIBindingType::UniformBufferDynamic) Binding.Type = ERHIBindingType::UniformBuffer;
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
		if (Prepared.bRejectBindings || !Input0 || Input0 == Prepared.Output.GetReference() || !Prepared.Output || !Prepared.Pipeline) return false;
		auto Scene = ResolveMaterialSceneTextureBindings(Commands, Prepared.Program->GetShaderMap().CompiledShaders[0].Reflection, Input0, Depth, Uniform);
		if (!Scene) return false;
		auto Bindings = Prepared.Bindings;
		Bindings.insert(Bindings.end(), Scene->Parameters.begin(), Scene->Parameters.end());
		FRHIRenderPassInfo Pass;
		Pass.RenderTargetLayout.NumColorRenderTargets = 1;
		Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,
			ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::DontCare);
		Pass.ColorRenderTargets[0] = Prepared.Output;
		Commands.BeginRenderPass(Pass, "Scene.PostProcessMaterial");
		Commands.SetGraphicsPipelineState(*Prepared.Pipeline);
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
		for (auto& Slot : State->Slots) Slot.reset();
		State->MaterialSlots.clear();
		State->MaterialOutput = nullptr;
		if (State->MaterialResources) State->MaterialResources->ReleaseResources_RenderThread();
	}
} // namespace Durin
