#include "Renderers/EditorAssistance/EditorGridRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "CoreGlobals.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		class FEditorGridVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FEditorGridVertexShader,
				FShader,
				"/Engine/EditorGrid",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FEditorGridFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FEditorGridFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Grid);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FEditorGridFragmentShader,
				FShader,
				"/Engine/EditorGrid",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};

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
		struct FBasePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FEditorGridVertexShader> VertexShader;
			TShaderRef<FEditorGridFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FGraphicsPipelineStateRHIRef> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		TRenderResourceCreationSlot<FBasePayload> Base{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
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

		using FBasePayload = FState::FBasePayload;
		using FBaseResult = TRenderResourceCreateResult<FBasePayload>;
		FBasePayload* Base = State->Base.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this]() -> FBaseResult {
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FEditorGridVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FEditorGridFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> ShaderTypes = {
					&VertexShaderType,
					&FragmentShaderType};

				FBasePayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(
						ShaderTypes, CompileOptions, ErrorMessage))
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"EditorGrid",
							"base",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				auto* VertexShader = static_cast<FEditorGridVertexShader*>(
					Candidate.ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader =
					static_cast<FEditorGridFragmentShader*>(
						Candidate.ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"EditorGrid",
							"base",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader = TShaderRef<FEditorGridVertexShader>(
					VertexShader, Candidate.ShaderMap.get());
				Candidate.FragmentShader =
					TShaderRef<FEditorGridFragmentShader>(
						FragmentShader, Candidate.ShaderMap.get());
				FVertexDeclarationElementList Elements;
				Elements[0] = FVertexElement(
					0,
					offsetof(FFullscreenGeometryResources::FVertex, Position),
					EVertexElementType::Float2,
					0,
					sizeof(FFullscreenGeometryResources::FVertex));
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);
				if (Candidate.VertexShader.GetRHIShader(false) == nullptr
					|| Candidate.FragmentShader.GetRHIShader(false) == nullptr
					|| Candidate.VertexDeclaration == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"EditorGrid",
							"base",
							"RHI shader or vertex declaration creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FBaseResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Base == nullptr)
			return;

		const FPipelineKey Key{
			.Feature = EFeature::EditorGrid,
			.Output = Output,
			.DepthMode = EDepthMode::Visible,
		};
		auto EntryIt = std::ranges::find(
			State->Pipelines, Key, &FState::FPipelineEntry::Key);
		if (EntryIt == State->Pipelines.end())
		{
			EntryIt = State->Pipelines.emplace(
				State->Pipelines.end(), FState::FPipelineEntry{.Key = Key});
		}
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			State->Base.GetPayloadGeneration().Shader;
		using FPipelineResult =
			TRenderResourceCreateResult<FGraphicsPipelineStateRHIRef>;
		auto* Pipeline = EntryIt->Slot.Resolve(
			PipelineGeneration,
			[Base, Key]() -> FPipelineResult {
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeEditorAssistanceOutput(
						Key.Output);
				Initializer.BoundShaders.VertexShader =
					Base->VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					Base->FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = Base->VertexDeclaration;
				Initializer.ColorBlendState =
					FRHIColorBlendState::StraightAlpha();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.DepthState.bEnableTest = true;
				Initializer.PipelineLayout =
					Base->ShaderMap->GetMergedPipelineLayout();
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
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Pipeline == nullptr)
			return;

		Prepared.EditorGridUniform = Uniform;
		Prepared.Pipelines.push_back({
			.Key = Key,
			.Pipeline = *Pipeline,
		});
	}

	auto FEditorGridRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		const FState::FBasePayload* Base = State->Base.GetPayload();
		const FGraphicsPipelineStateRHIRef Pipeline =
			FindPreparedPipeline(Prepared);
		if (!Prepared.EditorGridUniform.has_value()
			|| Base == nullptr
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
		SetShaderParameters(CommandList, Base->FragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
	}

	auto FEditorGridRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Base.Reset();
		for (FState::FPipelineEntry& Entry : State->Pipelines)
			Entry.Slot.Reset();
		State->Pipelines.clear();
	}
} // namespace Durin
