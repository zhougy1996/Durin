#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FGTAOVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FGTAOVertexShader, FShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Vertex, "VertexMain");
		};

		class FGTAORawFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAORawFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FGTAORawFragmentShader, FShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "RawFragmentMain");
		};

		class FGTAOFilterFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGTAOFilterFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(FilterInputVisibility);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterGBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(FilterSceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Filter);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FGTAOFilterFragmentShader, FShader,
				"/Engine/GroundTruthAmbientOcclusion",
				EShaderFrequency::Fragment, "BilateralFragmentMain");
		};
	}

	struct FGroundTruthAmbientOcclusionRenderer::FState
	{
		struct FPayload
		{
			std::shared_ptr<FShaderMapBase> RawShaderMap;
			std::shared_ptr<FShaderMapBase> FilterShaderMap;
			TShaderRef<FGTAOVertexShader> RawVertexShader;
			TShaderRef<FGTAOVertexShader> FilterVertexShader;
			TShaderRef<FGTAORawFragmentShader> FragmentShader;
			TShaderRef<FGTAOFilterFragmentShader> FilterFragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FGraphicsPipelineStateRHIRef FilterPipelineState;
		};

		TRenderResourceCreationSlot<FPayload> Resources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> TargetsBySize{
			ERenderResourceGenerationDependency::Device};
	};

	FGroundTruthAmbientOcclusionRenderer::FGroundTruthAmbientOcclusionRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, State(std::make_unique<FState>())
	{
	}

	FGroundTruthAmbientOcclusionRenderer::~FGroundTruthAmbientOcclusionRenderer() = default;

	auto FGroundTruthAmbientOcclusionRenderer::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		using FPayload = FState::FPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType = FGTAOVertexShader::StaticType();
				FShaderType& FragmentType = FGTAORawFragmentShader::StaticType();
				FShaderType& FilterType = FGTAOFilterFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> RawTypes{
					&VertexType, &FragmentType};
				const std::array<const FShaderType*, 2> FilterTypes{
					&VertexType, &FilterType};
				FPayload Candidate;
				Candidate.RawShaderMap = std::make_shared<FShaderMapBase>();
				Candidate.FilterShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Candidate.RawShaderMap->InitializeFromShaderTypes(
						RawTypes, Options, Error))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GroundTruthAmbientOcclusion", "raw-shader",
						std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				if (!Candidate.FilterShaderMap->InitializeFromShaderTypes(
						FilterTypes, Options, Error))
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GroundTruthAmbientOcclusion", "filter-shader",
						std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				auto* RawVertex = static_cast<FGTAOVertexShader*>(
					Candidate.RawShaderMap->GetShader(&VertexType));
				auto* FilterVertex = static_cast<FGTAOVertexShader*>(
					Candidate.FilterShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FGTAORawFragmentShader*>(
					Candidate.RawShaderMap->GetShader(&FragmentType));
				auto* Filter = static_cast<FGTAOFilterFragmentShader*>(
					Candidate.FilterShaderMap->GetShader(&FilterType));
				if (RawVertex == nullptr || FilterVertex == nullptr
					|| Fragment == nullptr || Filter == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"GroundTruthAmbientOcclusion", "raw-shader",
						"Compiled map omitted a typed GTAO shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.RawVertexShader = {
					RawVertex, Candidate.RawShaderMap.get()};
				Candidate.FilterVertexShader = {
					FilterVertex, Candidate.FilterShaderMap.get()};
				Candidate.FragmentShader = {
					Fragment, Candidate.RawShaderMap.get()};
				Candidate.FilterFragmentShader = {
					Filter, Candidate.FilterShaderMap.get()};
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
						"GroundTruthAmbientOcclusion", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				FRHIShader* RawVertexRHI =
					Candidate.RawVertexShader.GetRHIShader(false);
				FRHIShader* FilterVertexRHI =
					Candidate.FilterVertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				FRHIShader* FilterRHI =
					Candidate.FilterFragmentShader.GetRHIShader(false);
				if (Candidate.VertexDeclaration == nullptr
					|| RawVertexRHI == nullptr || FilterVertexRHI == nullptr
					|| FragmentRHI == nullptr
					|| FilterRHI == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GroundTruthAmbientOcclusion", "raw-shader",
						"RHI shader or vertex declaration creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
				Initializer.BoundShaders.VertexShader = RawVertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration = Candidate.VertexDeclaration;
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					Candidate.RawShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionRawPipeline", Initializer);
				if (Candidate.PipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "raw-pipeline",
						"RHI graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				Initializer.BoundShaders.VertexShader = FilterVertexRHI;
				Initializer.BoundShaders.FragmentShader = FilterRHI;
				Initializer.PipelineLayout =
					Candidate.FilterShaderMap->GetMergedPipelineLayout();
				Candidate.FilterPipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"GroundTruthAmbientOcclusionFilterPipeline", Initializer);
				if (Candidate.FilterPipelineState == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GroundTruthAmbientOcclusion", "filter-pipeline",
						"RHI graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		return Payload != nullptr;
	}

	auto FGroundTruthAmbientOcclusionRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> FTargets*
	{
		if (Width == 0 || Height == 0) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"GroundTruthAmbientOcclusionRaw", Width, Height,
			EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[Key, &Desc]() -> FResult {
				FTargets Candidate;
				Candidate.Raw = RHICreateTexture(Desc);
				auto ScratchDesc = Desc;
				ScratchDesc.DebugName = "GroundTruthAmbientOcclusionScratch";
				Candidate.Scratch = RHICreateTexture(ScratchDesc);
				if (Candidate.Raw == nullptr || Candidate.Scratch == nullptr)
				{
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GroundTruthAmbientOcclusionTarget", std::to_string(Key),
						"One or more R8_UNORM targets returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		const bool bResolved = Targets != nullptr;
		auto GetRetainedBytes = [this]() {
			return State->TargetsBySize.GetRetainedPayloadWeight(
				[](uint64 SizeKey, const FTargets&) {
					return CalculateTargetBytes(
						static_cast<uint32>(SizeKey >> 32),
						static_cast<uint32>(SizeKey));
				});
		};
		while (State->TargetsBySize.Num() > 1
			&& GetRetainedBytes() > MaximumRetainedBytes)
		{
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		}
		if (!bResolved) return nullptr;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained != nullptr ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FGroundTruthAmbientOcclusionRenderer::RenderRaw_RenderThread(
		FRHICommandListImmediate& CommandList,
		FTargets& Targets,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Depth,
		const FSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Raw == nullptr || Normals == nullptr || Surface == nullptr
			|| Depth == nullptr || View.ViewportWidth == 0
			|| View.ViewportHeight == 0)
			return false;
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		if (Payload == nullptr || Payload->PipelineState == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
			return false;

		FViewUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
				Uniform.WorldToViewRows[Row][Col] =
					static_cast<float>(View.ViewMatrix[Col][Row]);
			}
		}
		Uniform.Viewport = {
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight)};
		const FRHIUniformBufferRange ViewUniform =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (ViewUniform.Buffer == nullptr || ViewUniform.Size != sizeof(Uniform))
			return false;

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
		PassInfo.ColorRenderTargets[0] = Targets.Raw;
		PassInfo.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "GroundTruthAmbientOcclusionRawPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth),
			static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FGTAORawFragmentShader::FParameters Parameters;
		Parameters.GBufferNormals = Normals;
		Parameters.GBufferSurface = Surface;
		Parameters.SceneDepth = Depth;
		Parameters.View = ViewUniform;
		SetShaderParameters(CommandList, Payload->FragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return true;
	}

	auto FGroundTruthAmbientOcclusionRenderer::RenderFilter_RenderThread(
		FRHICommandListImmediate& CommandList,
		FTargets& Targets,
		FRHITexture* Normals,
		FRHITexture* Surface,
		FRHITexture* Depth,
		const FSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (Targets.Raw == nullptr || Targets.Scratch == nullptr
			|| Normals == nullptr || Surface == nullptr || Depth == nullptr
			|| View.ViewportWidth == 0 || View.ViewportHeight == 0)
			return false;
		if (!EnsureResources_RenderThread(CommandList)) return false;
		FState::FPayload* Payload = State->Resources.GetPayload();
		if (Payload == nullptr || Payload->FilterPipelineState == nullptr
			|| FullscreenGeometry.GetVertexBuffer_RenderThread() == nullptr
			|| FullscreenGeometry.GetIndexBuffer_RenderThread() == nullptr)
			return false;

		FFilterUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.ProjectionRows[Row][Col] =
					static_cast<float>(View.ProjectionMatrix[Col][Row]);
			}
		}
		Uniform.Viewport = {
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight)};

		auto RenderDirection = [&](FRHITexture* Input, FRHITexture* Output,
			float DirectionX, float DirectionY, const char* PassName) {
			Uniform.DirectionAndThresholds = {
				DirectionX, DirectionY, 0.90f, 0.01f};
			const FRHIUniformBufferRange FilterUniform =
				CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			if (FilterUniform.Buffer == nullptr
				|| FilterUniform.Size != sizeof(Uniform))
				return false;
			FRHIRenderPassInfo PassInfo{};
			PassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeGroundTruthAmbientOcclusionOutput();
			PassInfo.ColorRenderTargets[0] = Output;
			PassInfo.ColorClearValues[0] =
				FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
			CommandList.BeginRenderPass(PassInfo, PassName);
			CommandList.SetGraphicsPipelineState(*Payload->FilterPipelineState);
			CommandList.SetViewport(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY), 0.0f,
				static_cast<float>(View.ViewportX + View.ViewportWidth),
				static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
			CommandList.SetScissor(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY),
				static_cast<float>(View.ViewportWidth),
				static_cast<float>(View.ViewportHeight));
			CommandList.BindVertexBuffer(0,
				FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
			CommandList.BindIndexBuffer(
				FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
			FGTAOFilterFragmentShader::FParameters Parameters;
			Parameters.FilterInputVisibility = Input;
			Parameters.FilterGBufferNormals = Normals;
			Parameters.FilterGBufferSurface = Surface;
			Parameters.FilterSceneDepth = Depth;
			Parameters.Filter = FilterUniform;
			SetShaderParameters(
				CommandList, Payload->FilterFragmentShader, Parameters);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
			return true;
		};

		return RenderDirection(
			Targets.Raw, Targets.Scratch, 1.0f, 0.0f,
			"GroundTruthAmbientOcclusionHorizontalFilter")
			&& RenderDirection(
				Targets.Scratch, Targets.Raw, 0.0f, 1.0f,
				"GroundTruthAmbientOcclusionVerticalFilter");
	}

	auto FGroundTruthAmbientOcclusionRenderer::ReleaseResources_RenderThread()
		-> void
	{
		State->Resources.Reset();
		State->TargetsBySize.Reset();
	}
}
