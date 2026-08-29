#include "Renderers/EditorAssistance/EditorGridRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
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

	namespace
	{
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

		auto FindPreparedPipeline(const FPrepared& Prepared)
			-> FGraphicsPipelineStateRHIRef
		{
			const auto It = std::ranges::find_if(
				Prepared.Pipelines,
				[](const FPreparedPipeline& Pipeline) {
					return Pipeline.Key.Feature == EFeature::EditorGrid
						&& Pipeline.Key.DepthMode == EDepthMode::Visible;
				});
			return It != Prepared.Pipelines.end() ? It->Pipeline : nullptr;
		}
	} // namespace

	struct FEditorGridRenderer::FState
	{
		struct FPipelinePayload
		{
			FGraphicsPipelineStateRHIRef Pipeline;
			FGlobalShaderSetRef ShaderSet;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FPipelinePayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		std::vector<FPipelineEntry> Pipelines;
	};

	FEditorGridRenderer::FEditorGridRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FEditorGridRenderer::~FEditorGridRenderer() = default;

	auto FEditorGridRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		EditorGridRendering::FEditorGridUniform Uniform;
		if (!EditorGridRendering::BuildUniform(View, Uniform)
			|| !FullscreenGeometry.EnsureResources_RenderThread(CommandList))
		{
			return;
		}

		const std::array<const FGlobalShaderType*, 2> ShaderTypes = {
			&FEditorGridVertexShader::StaticType(),
			&FEditorGridFragmentShader::StaticType()};
		FGlobalShaderSetRef ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
			"EditorAssistance.EditorGrid", ShaderTypes, true,
			ReportRendererResourceCreateDiagnostic);
		if (!ShaderSet)
			return;
		TShaderMapRef<FEditorGridVertexShader> VertexShader(ShaderSet);
		TShaderMapRef<FEditorGridFragmentShader> FragmentShader(ShaderSet);

		const FPipelineKey Key{
			.Feature = EFeature::EditorGrid,
			.Output = Output,
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
		FRenderResourceGeneration PipelineGeneration = ShaderSet.GetGeneration();
		using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
		auto* Pipeline = EntryIt->Slot.Resolve(
			PipelineGeneration,
			[this, ShaderSet, VertexShader, FragmentShader, Key]() -> FPipelineResult {
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeEditorAssistanceOutput(
						Key.Output);
				Initializer.BoundShaders.VertexShader =
					VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.ColorBlendStates[0] =
					FRHIColorBlendState::StraightAlpha();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.DepthStencilState.bEnableTest = true;
				Initializer.DepthStencilState.CompareOp =
					GetVisibleDepthCompareOp(Key.DepthConvention);
				Initializer.PipelineLayout =
					ShaderSet.GetPipelineLayout();
				const std::string PipelineName = std::format(
					"EditorGridVisible{}Pipeline",
					Key.Output
							== RenderTargetLayouts::EViewportOutput::Present
						? "Present"
						: "Offscreen");
				FGraphicsPipelineStateRHIRef Candidate =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						FName(PipelineName), Initializer);
				if (Candidate == nullptr)
				{
					return FPipelineResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"EditorGrid",
							PipelineName,
							"RHI graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FPipelineResult::Success({
					.Pipeline = std::move(Candidate),
					.ShaderSet = ShaderSet});
			},
			ReportRendererResourceCreateDiagnostic);
		if (Pipeline == nullptr)
			return;

		Prepared.EditorGridUniform = Uniform;
		Prepared.Pipelines.push_back({
			.Key = Key,
			.Pipeline = Pipeline->Pipeline,
			.ShaderSet = Pipeline->ShaderSet,
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
		const FGraphicsPipelineStateRHIRef Pipeline =
			PreparedIt != Prepared.Pipelines.end() ? PreparedIt->Pipeline : nullptr;
		if (!Prepared.EditorGridUniform.has_value()
			|| PreparedIt == Prepared.Pipelines.end()
			|| Pipeline == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
		{
			return;
		}
		CommandList.SetGraphicsPipelineState(*Pipeline);
		CommandList.BindVertexBuffer(
			0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FEditorGridFragmentShader::FParameters Parameters;
		Parameters.Grid = CommandList.AllocateDynamicUniformBuffer(
			&*Prepared.EditorGridUniform,
			sizeof(*Prepared.EditorGridUniform));
		SetShaderParameters(CommandList,
			TShaderMapRef<FEditorGridFragmentShader>(PreparedIt->ShaderSet).GetShaderRef(),
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
