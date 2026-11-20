#include "Renderers/EditorAssistance/EditorGridRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "CoreGlobals.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	class FEditorGridVertexShader : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(
			FEditorGridVertexShader,
			FGlobalShader,
			"/Engine/EditorGrid",
			EShaderFrequency::Vertex,
			"VertexMain");
	};

	class FEditorGridFragmentShader : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FEditorGridFragmentShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Grid);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_GLOBAL_SHADER(
			FEditorGridFragmentShader,
			FGlobalShader,
			"/Engine/EditorGrid",
			EShaderFrequency::Fragment,
			"FragmentMain");
	};

	DURIN_IMPLEMENT_GLOBAL_SHADER(FEditorGridVertexShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FEditorGridFragmentShader);
	namespace
	{
		const FGlobalShaderSetRegistration GEditorGridShaderSet(
			"Renderer", "EditorAssistance.EditorGrid",
			EShaderRequestEligibility::EditorOnly,
			{&FEditorGridVertexShader::StaticType(),
			 &FEditorGridFragmentShader::StaticType()});

	} // namespace

	struct FEditorGridRenderer::FState
	{
		struct FPipelinePayload
		{
			FVertexDeclarationRHIRef PipelineVertexDeclaration;
			FGraphicsPipelineStateInitializer PipelineInitializer;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FPipelinePayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual};
		};

		std::vector<FPipelineEntry> Pipelines;
	};

	FEditorGridRenderer::FEditorGridRenderer(
		FFullscreenGeometryResources& InFullscreenGeometry)
		: FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FEditorGridRenderer::~FEditorGridRenderer() = default;

	auto FEditorGridRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		EPixelFormat OutputFormat,
		FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		EditorGridRendering::FEditorGridUniform Uniform;
		if (!EditorGridRendering::BuildUniform(View, Uniform)
			|| !FullscreenGeometry.EnsureResources_RenderThread(CommandList))
		{
			return;
		}

		TShaderMapRef<FEditorGridVertexShader> VertexShader(GetGlobalShaderMap());
		TShaderMapRef<FEditorGridFragmentShader> FragmentShader(GetGlobalShaderMap());

		if (!VertexShader.GetRHIShader(false) || !FragmentShader.GetRHIShader(false)) return;

		const FPipelineKey Key{
			.Feature = EFeature::EditorGrid,
			.Output = Output,
			.OutputFormat = OutputFormat,
			.DepthMode = EDepthMode::Visible,
			.DepthConvention = View.DepthConvention,
		};
		auto EntryIt = std::ranges::find(
			State->Pipelines, Key, &FState::FPipelineEntry::Key);
		if (EntryIt == State->Pipelines.end())
		{
			EntryIt = State->Pipelines.emplace(
				State->Pipelines.end(), FState::FPipelineEntry{.Key = Key});
		}

		using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
		auto* Pipeline = EntryIt->Slot.Resolve(
			GetGlobalShaderMap().GetGeneration_RenderThread(),
			[this, VertexShader, FragmentShader, Key]() -> FPipelineResult {
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeEditorAssistanceOutput(
						Key.Output, Key.OutputFormat);
				Initializer.BoundShaders.VertexShader =
					VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.BlendState = RHICreateBlendState({FRHIColorBlendState::StraightAlpha()});
				Initializer.RasterizerState = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
				Initializer.DepthStencilState = RHICreateDepthStencilState({
					.bEnableTest = true,
					.CompareOp = GetVisibleDepthCompareOp(Key.DepthConvention)});
				Initializer.PipelineLayout =
					FragmentShader.GetPipelineLayout();

				return FPipelineResult::Success({
					.PipelineVertexDeclaration = Initializer.VertexDeclaration,
					.PipelineInitializer = Initializer});
			},
			ReportRendererResourceCreateDiagnostic);
		if (!Pipeline)
			return;

		Prepared.EditorGridUniform = Uniform;
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Pipeline->PipelineInitializer, "EditorGridPipeline");
			if (!Request) return;
			FRenderPipelinePreparationBatch::Add(Request);
			if (Request->GetState() == ERHIPipelineState::Failed || Request->GetState() == ERHIPipelineState::Canceled) return;
		}
		Prepared.Pipelines.push_back({
			.Key = Key,
			.PipelineVertexDeclaration = Pipeline->PipelineVertexDeclaration,
			.PipelineInitializer = Pipeline->PipelineInitializer,
			.GridFragmentShader = FragmentShader,
		});
	}

	auto FEditorGridRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		const auto PreparedIt = std::ranges::find_if(
			Prepared.Pipelines, [](const FPreparedPipeline& Item) {
				return Item.Key.Feature == EFeature::EditorGrid;
			});
		const auto* Pipeline =
			PreparedIt != Prepared.Pipelines.end() ? &PreparedIt->PipelineInitializer : nullptr;
		if (!Prepared.EditorGridUniform.has_value()
			|| PreparedIt == Prepared.Pipelines.end()
			|| !Pipeline
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return;
		}
		SetGraphicsPipelineState(CommandList, *Pipeline);
		CommandList.BindVertexBuffer(
			0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FEditorGridFragmentShader::FParameters Parameters;
		Parameters.Grid = CommandList.CreateUniformBufferRange(
			&*Prepared.EditorGridUniform,
			sizeof(*Prepared.EditorGridUniform));
		SetShaderParameters(CommandList,
			PreparedIt->GridFragmentShader,
			Parameters);
		CommandList.DrawIndexed(3, 0, 0);
	}

	auto FEditorGridRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		for (FState::FPipelineEntry& Entry : State->Pipelines)
			Entry.Slot.Reset();
		State->Pipelines.clear();
	}
} // namespace Durin
