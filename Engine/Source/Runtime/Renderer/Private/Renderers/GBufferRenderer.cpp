#include "Renderers/SurfaceMaterial.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshVertexFactory.h"

#include "Renderers/MeshRendererShared.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "RHI.h"
#include "Shader/MaterialShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{

		class FGBufferFragmentShader final : public RendererPrivate::FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MeshView);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MATERIAL_SHADER(FGBufferFragmentShader, RendererPrivate::FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"GeometryFragmentMain");
		};

		DURIN_IMPLEMENT_MATERIAL_SHADER(FGBufferFragmentShader);

		struct FGBufferShaderMapKey
		{
			FMaterialShaderMapIdentity Material;
			FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
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
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
			auto operator==(const FGBufferPipelineKey&) const -> bool = default;
		};

		auto GetGBufferPlanningPassIdentity(const FGBufferPipelineKey& Key)
			-> std::string
		{
			return std::format(
				"layout-version={},layout-id={},program={},blend={},shading={},domain={},declaration={}",
				Key.Material.ShaderMap.RenderLayout.Version,
				Key.Material.ShaderMap.RenderLayout.Id.ToString(),
				Key.Material.ShaderMap.ProgramIdentity.ToString(),
				static_cast<uint8>(Key.Material.ShaderMap.BlendMode),
				static_cast<uint8>(Key.Material.ShaderMap.ShadingModel),
				Key.FactoryKey.HashValue,
				reinterpret_cast<uintptr_t>(
					Key.VertexDeclaration.GetReference()));
		}
	} // namespace

	struct FGBufferPipeline
	{
		FMaterialShaderMap ShaderMap;
		std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding> Vertex;
		TMaterialShaderRef<FGBufferFragmentShader> Fragment;
		FShaderRHIRef FragmentRHI;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
		FXxHash64 FactoryKey;
			FXxHash64 LayoutKey;
	};

	struct FGBufferRenderer::FState
	{
		struct FShaderMapPayload
		{
			FMaterialShaderMap ShaderMap;
			std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding> Vertex;
			TMaterialShaderRef<FGBufferFragmentShader> Fragment;
		};

		TRendererResourceSlotCache<FGBufferShaderMapKey, FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FGBufferPipelineKey, std::shared_ptr<const FPipeline>>
			Pipelines{ERenderResourceGenerationDependency::Shader
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
		if (Request.VertexDeclaration == nullptr
			|| Request.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Translucent
			|| Request.Material.ShaderMap.ShadingModel
				!= EMaterialShadingModel::Lit)
		{
			return nullptr;
		}

		const auto Factory = RendererPrivate::FindMeshVertexFactory(Request.FactoryKey);
		if (!Factory || Factory->GetLayoutKey() != Request.LayoutKey
			|| !Factory->GetShaderType(RendererPrivate::MaterialMeshPassGBuffer)) return nullptr;
		const FGBufferShaderMapKey ShaderKey{
			.Material = Request.Material.ShaderMap,
			.FactoryKey = Request.FactoryKey, .LayoutKey = Request.LayoutKey};
		auto& ShaderEntry = State->ShaderMaps.FindOrAddBounded(
			ShaderKey, RendererPrivate::MaterialShaderMapCacheEntryBudget);
		using FShaderResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		FState::FShaderMapPayload* Shaders = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, ShaderKey, Factory, CompiledProgram = Request.CompiledProgram]() -> FShaderResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				Options.Macros.emplace_back(
					"DURIN_MATERIAL_BLEND_MODE",
					std::to_string(static_cast<uint8>(
						ShaderKey.Material.BlendMode)));
				Options.Macros.emplace_back(
					"DURIN_MATERIAL_SHADING_MODEL",
					std::to_string(static_cast<uint8>(
						ShaderKey.Material.ShadingModel)));
				Options.Macros.emplace_back(
					"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
					std::to_string(std::bit_cast<uint32>(
						ShaderKey.Material.OpacityMaskThreshold)));
				FShaderType* VertexType = Factory->GetShaderType(RendererPrivate::MaterialMeshPassGBuffer);
				FShaderType& FragmentType =
					FGBufferFragmentShader::StaticType();
				FMaterialShaderMap ShaderMap;

				auto ShaderResult = RendererPrivate::InitializeMaterialShaderMap(
					*VertexType, FragmentType,
					Factory->GetType(),
					RendererPrivate::MaterialMeshPassGBuffer, ShaderKey.Material,
					Coordinator.GetGeneration_RenderThread(), CompiledProgram.get(),
					Options, ShaderMap);
				if (!ShaderResult)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"GBufferShaderMap",
							std::to_string(ShaderKey.FactoryKey.HashValue),
							std::move(ShaderResult.error()),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				FState::FShaderMapPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				Candidate.Vertex = Factory->Resolve(Candidate.ShaderMap, RendererPrivate::MaterialMeshPassGBuffer);
				Candidate.Fragment =
					TMaterialShaderRef<FGBufferFragmentShader>(Candidate.ShaderMap);
				FRHIShader* VertexRHI = Candidate.Vertex ? Candidate.Vertex->GetRHIShader(false) : nullptr;
				if (VertexRHI == nullptr
					|| Candidate.Fragment.GetRHIShader(false) == nullptr)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"GBufferShaderMap",
							std::to_string(ShaderKey.FactoryKey.HashValue),
							ERenderResourceCreateErrorReason::ShaderCreationFailed,
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FShaderResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Shaders == nullptr) return nullptr;

		const FGBufferPipelineKey PipelineKey{
			.Material = Request.Material,
			.Rasterizer = Request.Rasterizer,
			.Depth = Request.Depth,
			.VertexDeclaration = Request.VertexDeclaration,
			.FactoryKey = Request.FactoryKey, .LayoutKey = Request.LayoutKey, .Topology = Request.Topology};
		auto& PipelineEntry = State->Pipelines.FindOrAddBounded(
			PipelineKey, RendererPrivate::MaterialPipelineCacheEntryBudget);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader = Shaders->ShaderMap.GetGeneration().Shader;
		using FPipelineResult =
			TRenderResourceCreateResult<std::shared_ptr<const FPipeline>>;
		std::shared_ptr<const FPipeline>* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, Shaders, PipelineKey,
			 VertexDeclaration = Request.VertexDeclaration]()
				-> FPipelineResult {
				auto Candidate = std::make_shared<FPipeline>();
				Candidate->ShaderMap = Shaders->ShaderMap;
				Candidate->Vertex = Shaders->Vertex;
				Candidate->Fragment = Shaders->Fragment;
				Candidate->FragmentRHI = Candidate->Fragment.GetRHIShader();
				Candidate->VertexDeclaration = VertexDeclaration;
				Candidate->FactoryKey = PipelineKey.FactoryKey;
				Candidate->LayoutKey = PipelineKey.LayoutKey;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferTargets();
				Initializer.BoundShaders.VertexShader = Candidate->Vertex->GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					Candidate->Fragment.GetRHIShader();
				Initializer.VertexDeclaration = VertexDeclaration;
				Initializer.PrimitiveTopology = PipelineKey.Topology;
				Initializer.RasterizerState = PipelineKey.Rasterizer;
				Initializer.DepthStencilState = PipelineKey.Depth;
				Initializer.PipelineLayout =
					Candidate->ShaderMap.GetPipelineLayout();
				Candidate->PipelineState =
					FRenderPipelineRequestScope::Graphics(
						FName(std::format("GBufferPipeline_{}",
							PipelineEntry.Index)),
						Initializer);
				if (Candidate->PipelineState == nullptr)
				{
					return FPipelineResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"GBufferPipeline",
							GetGBufferPlanningPassIdentity(PipelineKey),
							ERenderResourceCreateErrorReason::PipelineCreationFailed,
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
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
		if (Pipeline.PipelineState == nullptr || FragmentBindings.GetShader() != Pipeline.FragmentRHI) return false;
		if (!VertexBindings || VertexBindings->GetShader() != Pipeline.Vertex->GetRHIShader(false)) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline.PipelineState);
		CommandList.SetPreparedShaderParameters(VertexBindings);

		return FragmentBindings.Bind(CommandList);
	}

	auto FGBufferRenderer::GetVertexBinding(const FPipeline& Pipeline, const FVertexFactoryBinding& Binding) const
		-> std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding>
	{
		if (Binding.GetFactoryKey() != Pipeline.FactoryKey || Binding.GetLayoutKey() != Pipeline.LayoutKey) return {};
		return Pipeline.Vertex;
	}

	auto FGBufferRenderer::GetFragmentShader(const FPipeline& Pipeline) const -> FRHIShader*
	{
		return Pipeline.FragmentRHI;
	}

	auto FGBufferRenderer::GetSurfaceLayout(const FPipeline& Pipeline) const
		-> const RendererPrivate::FCompiledSurfaceBindingLayout&
	{
		return Pipeline.Fragment.GetShader()->GetSurfaceLayout();
	}

	auto FGBufferRenderer::ReleaseResources_RenderThread() -> void
	{
		State->ShaderMaps.Reset();
		State->Pipelines.Reset();
	}
} // namespace Durin
