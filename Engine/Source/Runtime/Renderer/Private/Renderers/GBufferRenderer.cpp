#include "Renderers/SurfaceMaterial.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/MeshVertexFactory.h"

#include "Renderers/MeshRendererShared.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "RHI.h"
#include "Shader/MaterialShader.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"
#include "RDG/RDGParameters.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{

		class FGBufferFragmentShader final : public RendererPrivate::FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MaterialView);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MaterialPrimitive);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MATERIAL_SHADER(FGBufferFragmentShader, RendererPrivate::FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"GeometryFragmentMain");
		};

		DURIN_IMPLEMENT_MATERIAL_SHADER(FGBufferFragmentShader);

		class FGBufferGPUCullingComputeShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferGPUCullingComputeShader)
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(Candidates);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(VisibleInstances);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(IndirectArguments);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FGBufferGPUCullingComputeShader,
				FGlobalShader, "/Engine/GBufferGPUCulling",
				EShaderFrequency::Compute, "CullMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGBufferGPUCullingComputeShader);
		const FGlobalShaderSetRegistration GGBufferGPUCullingShaderSet(
			"Renderer", "GBuffer.GPUCulling",
			EShaderRequestEligibility::GameAndEditor,
			{&FGBufferGPUCullingComputeShader::StaticType()});

		struct FGBufferShaderMapKey
		{
			FMeshMaterialShaderMapIdentity Material;
			FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
			bool bGPUCulling = false;
			auto operator==(const FGBufferShaderMapKey&) const -> bool = default;
		};

		struct FGBufferPipelineKey
		{
			FMaterialPlanningPassIdentity Material;
			FRHIRasterizerState Rasterizer;
			FRHIDepthStencilState Depth;
			FVertexDeclarationRHIRef VertexDeclaration;
			FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
			bool bGPUCulling = false;
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
			auto operator==(const FGBufferPipelineKey&) const -> bool = default;
		};

		auto GetGBufferPlanningPassIdentity(const FGBufferPipelineKey& Key)
			-> std::string
		{
			return std::format(
				"layout-version={},layout-id={},program={},blend={},shading={},domain={},gpu-culling={},declaration={}",
				Key.Material.ShaderMap.RenderLayout.Version,
				Key.Material.ShaderMap.RenderLayout.Id.ToString(),
				Key.Material.ShaderMap.ProgramIdentity.ToString(),
				static_cast<uint8>(Key.Material.ShaderMap.BlendMode),
				static_cast<uint8>(Key.Material.ShaderMap.ShadingModel),
				Key.FactoryKey.HashValue,
				Key.bGPUCulling,
				reinterpret_cast<uintptr_t>(
					Key.VertexDeclaration.GetReference()));
		}
	} // namespace

	struct FGBufferPipeline : RendererPrivate::FMeshPassPipeline {};

	struct FGBufferRenderer::FState
	{
		struct FGPUCullingPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FGBufferGPUCullingComputeShader> Shader;
			FComputePipelineStateRHIRef Pipeline;
		};
		using FShaderMapPayload = RendererPrivate::FMeshPassShaders;

		TRendererResourceSlotCache<FGBufferShaderMapKey, FShaderMapPayload, RendererPrivate::FMaterialResourceCacheKeyHash>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FGBufferPipelineKey, std::shared_ptr<const FPipeline>, RendererPrivate::FMaterialResourceCacheKeyHash>
			Pipelines{ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FGPUCullingPayload> GPUCulling{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FGBufferRenderer::FGBufferRenderer(FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FGBufferRenderer::~FGBufferRenderer() = default;

	auto FGBufferRenderer::DescribeTargets(uint32 Width, uint32 Height)
		-> std::array<FRHITextureCreateDesc, 4>
	{
		auto MakeDesc = [Width, Height](const char* Name, EPixelFormat Format) {
			return FRHITextureCreateDesc::Create2D(Name, Width, Height, Format)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy)
				.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f));
		};
		return {
			MakeDesc("GBufferMaterial", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferNormals", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferSurface", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferEmissive", EPixelFormat::R11G11B10_FLOAT)};
	}
	auto FGBufferRenderer::EnsurePipeline_RenderThread(
		const FPipelineRequest& Request) -> std::shared_ptr<const FPipeline>
	{
		if (Request.bGPUCulling && Request.Failure
			== EGPUCullingFailure::PipelineUnavailable) return nullptr;
		if (Request.VertexDeclaration == nullptr) return nullptr;
		const auto Factory = RendererPrivate::FindMeshVertexFactory(Request.FactoryKey);
		if (!Factory || Factory->GetLayoutKey() != Request.LayoutKey) return nullptr;
		const auto& Processor = RendererPrivate::FPassProcessorManager::GetMeshPassProcessor(RendererPrivate::EMeshPipelinePass::GBuffer);
		const RendererPrivate::FMeshPassProcessorBatch Batch{Request.Material, *Factory,
			{Request.Rasterizer, Request.Depth, {}}, Request.Topology, {}, true, Request.bGPUCulling};
		const FGBufferShaderMapKey ShaderKey{
			.Material = Request.Material.ShaderMap,
			.FactoryKey = Request.FactoryKey, .LayoutKey = Request.LayoutKey,
			.bGPUCulling = Request.bGPUCulling};
		auto& ShaderEntry = State->ShaderMaps.FindOrAddBounded(
			ShaderKey, RendererPrivate::MaterialShaderMapCacheEntryBudget, RendererPrivate::MaterialShaderMapCacheByteBudget);
		using FShaderResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		FState::FShaderMapPayload* Shaders = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, ShaderKey, &Processor, &Batch, CompiledProgram = Request.CompiledProgram]() -> FShaderResult {
				FState::FShaderMapPayload Candidate;
				FShaderCompileOptions Options;
				Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
				RendererPrivate::FMeshPassDrawListContext Context(Coordinator.GetGeneration_RenderThread(), CompiledProgram.get(), Options, Candidate);
				Processor.AddMeshBatch(Batch, Context);
				const auto& Result = Context.GetResult();
				if (Result.Status == EShaderBuildStatus::Pending) return FShaderResult::Pending();
				if (Result.Status == EShaderBuildStatus::Canceled) return FShaderResult::Canceled();
				if (!Result) return FShaderResult::Failure(MakeRendererResourceCreateError(
					Context.GetFailureCategory(), "GBufferShaderMap", std::to_string(ShaderKey.FactoryKey.HashValue), Result.Error,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual
						| (Context.GetFailureCategory() == ERenderResourceCreateErrorCategory::RHIResource
							? ERenderResourceGenerationDependency::Device : ERenderResourceGenerationDependency::None)));
				return FShaderResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Shaders == nullptr) return nullptr;

		const FGBufferPipelineKey PipelineKey{
			.Material = Request.Material,
			.Rasterizer = Request.Rasterizer,
			.Depth = Request.Depth,
			.VertexDeclaration = Request.VertexDeclaration,
			.FactoryKey = Request.FactoryKey, .LayoutKey = Request.LayoutKey,
			.bGPUCulling = Request.bGPUCulling, .Topology = Request.Topology};
		auto& PipelineEntry = State->Pipelines.FindOrAddBounded(
			PipelineKey, RendererPrivate::MaterialPipelineCacheEntryBudget, RendererPrivate::MaterialPipelineCacheByteBudget);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader = Shaders->ShaderMap.GetGeneration().Shader;
		using FPipelineResult =
			TRenderResourceCreateResult<std::shared_ptr<const FPipeline>>;
		std::shared_ptr<const FPipeline>* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, Shaders, PipelineKey, &Processor, &Batch, VertexDeclaration = Request.VertexDeclaration]() -> FPipelineResult {
				auto Candidate = std::make_shared<FPipeline>();
				RendererPrivate::FMeshPassDrawListContext Context(FName(std::format("GBufferPipeline_{}", PipelineEntry.Index)),
					*Shaders, VertexDeclaration, *Candidate);
				Processor.AddMeshBatch(Batch, Context);
				if (!Context.GetResult()) return FPipelineResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "GBufferPipeline", GetGBufferPlanningPassIdentity(PipelineKey),
					ERenderResourceCreateErrorReason::PipelineCreationFailed,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
				Candidate->FactoryKey = PipelineKey.FactoryKey;
				Candidate->LayoutKey = PipelineKey.LayoutKey;
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		return Pipeline != nullptr ? *Pipeline : nullptr;
	}

	auto FGBufferRenderer::BindPipeline_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPipeline& Pipeline,
		const std::shared_ptr<const FRHIShaderParameterBatch>& VertexBindings,
		const RendererPrivate::FPreparedSurfaceMaterialBindings& FragmentBindings) -> bool
	{
		if (Pipeline.PipelineState == nullptr || FragmentBindings.GetShader() != Pipeline.FragmentShader) return false;
		if (!VertexBindings || VertexBindings->GetShader() != Pipeline.VertexShader->GetRHIShader(false)) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline.PipelineState);
		CommandList.SetPreparedShaderParameters(VertexBindings);

		return FragmentBindings.Bind(CommandList);
	}

	auto FGBufferRenderer::GetVertexBinding(const FPipeline& Pipeline, const FVertexFactoryBinding& Binding) const
		-> std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding>
	{
		if (Binding.GetFactoryKey() != Pipeline.FactoryKey || Binding.GetLayoutKey() != Pipeline.LayoutKey) return {};
		return Pipeline.VertexShader;
	}

	auto FGBufferRenderer::GetFragmentShader(const FPipeline& Pipeline) const -> FRHIShader*
	{
		return Pipeline.FragmentShader;
	}

	auto FGBufferRenderer::GetSurfaceLayout(const FPipeline& Pipeline) const
		-> const RendererPrivate::FCompiledSurfaceBindingLayout&
	{
		return *Pipeline.FragmentLayout;
	}

	auto FGBufferRenderer::EnsureGPUCullingResources_RenderThread(EGPUCullingFailure Failure) -> bool
	{
		if (Failure == EGPUCullingFailure::ShaderUnavailable) return false;
		using FPayload = FState::FGPUCullingPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return State->GPUCulling.Resolve(
			Coordinator.GetGeneration_RenderThread(), []() -> FResult {
				const std::array<const FGlobalShaderType*, 1> Types{
					&FGBufferGPUCullingComputeShader::StaticType()};
				FPayload Candidate;
				const auto ShaderSetCompletion = GetGlobalShaderMap().ResolveShaderSet(
					"GBuffer.GPUCulling", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (ShaderSetCompletion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
				if (ShaderSetCompletion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
				if (!ShaderSetCompletion)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"GBufferGPUCulling", "shader",
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				Candidate.ShaderSet = *ShaderSetCompletion;
				Candidate.Shader = TShaderMapRef<FGBufferGPUCullingComputeShader>(
					Candidate.ShaderSet);
				FRHIShader* ComputeRHI = Candidate.Shader.GetRHIShader(false);
				if (ComputeRHI == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"GBufferGPUCulling", "pipeline",
						ERenderResourceCreateErrorReason::ShaderCreationFailed,
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FComputePipelineStateInitializer Initializer;
				Initializer.ComputeShader = ComputeRHI;
				Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
				Candidate.Pipeline = FRenderPipelineRequestScope::Compute(
					"GBufferGPUCullingPipeline", Initializer);
				if (Candidate.Pipeline == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"GBufferGPUCulling", "pipeline",
						ERenderResourceCreateErrorReason::PipelineCreationFailed,
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable)
			!= nullptr;
	}

	auto FGBufferRenderer::DispatchGPUCulling_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		uint32 CandidateCount,
		const FRDGShaderParameterScope& GraphShaderParameters, EGPUCullingFailure Failure) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		if (Failure == EGPUCullingFailure::ShaderUnavailable
			|| Failure == EGPUCullingFailure::CommandAdmission) return false;
		auto* Payload = State->GPUCulling.GetPayload();
		if (Payload == nullptr || CandidateCount == 0) return false;
		FGBufferGPUCullingUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Column = 0; Column < 4; ++Column)
				Uniform.ViewProjection[Row * 4 + Column] = static_cast<float>(
					View.GetTranslatedWorldToClip()[Column][Row]);
		Uniform.CandidateCount = CandidateCount;
		const auto UniformBuffer = CommandList.CreateUniformBufferRange(
			&Uniform, sizeof(Uniform));
		if (!UniformBuffer.Buffer) return false;
		CommandList.SwitchPipeline(ERHIPipeline::Compute);
		CommandList.SetComputePipelineState(*Payload->Pipeline);
		FGBufferGPUCullingComputeShader::FParameters Parameters;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->Shader,
			GraphShaderParameters, Parameters);
		CommandList.Dispatch((CandidateCount + 63u) / 64u, 1, 1);
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		return true;
	}

	auto FGBufferRenderer::ReleaseResources_RenderThread() -> void
	{
		State->ShaderMaps.Reset();
		State->Pipelines.Reset();
		State->GPUCulling.Reset();
	}
} // namespace Durin

namespace Durin::RendererPrivate
{
	namespace
	{
		class FGBufferPassMeshProcessor final : public FMeshPassProcessor
		{
		public:
			using FMeshPassProcessor::FMeshPassProcessor;
			auto AddMeshBatch(const FMeshPassProcessorBatch& Batch, FMeshPassDrawListContext& Context) const -> void override
			{ BuildMeshDrawCommands(Batch, Context); }
			auto GetPass() const -> EMeshPipelinePass override { return EMeshPipelinePass::GBuffer; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override { return Config.Settings.bHybrid; }
			auto GetVertexFactoryPass(bool bGPUCulling) const -> uint32 override { return bGPUCulling ? MaterialMeshPassGBufferGPUCulling : MaterialMeshPassGBuffer; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override {
				FMeshPassFragmentShader Result{&FGBufferFragmentShader::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<FGBufferFragmentShader>(Map).GetRHIShader(false); }};
				Result.ResolveLayout = +[](const FMeshMaterialShaderMap& Map) -> const FCompiledSurfaceBindingLayout* { return &TMaterialShaderRef<FGBufferFragmentShader>(Map).GetShader()->GetSurfaceLayout(); };
				return Result;
			}
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 8); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey, bool) const -> FMeshPipelineDescription override
			{ return {.RenderTargets = RenderTargetLayouts::MakeGBufferTargets(), .Topology = Topology, .Rasterizer = State.Rasterizer, .Depth = State.Depth}; }
		};
		auto CreateGBufferMeshPassProcessor(FMeshPSOCollectorContext Context) -> std::unique_ptr<FMeshPassProcessor>
		{ return std::make_unique<FGBufferPassMeshProcessor>(Context); }
		const FRegisterPassProcessorCreateFunction RegisterGBufferMeshPass(&CreateGBufferMeshPassProcessor, EMeshPipelinePass::GBuffer);

	}
}
