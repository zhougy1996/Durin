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

		class FGBufferFragmentShader final : public FMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(BaseColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(NormalTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(MetallicTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(RoughnessTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(AmbientOcclusionTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EmissiveTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(OpacityTexture);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(BaseColorSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(NormalSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(MetallicSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(RoughnessSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(AmbientOcclusionSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(EmissiveSampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(OpacitySampler);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(OpacityMaskSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MATERIAL_SHADER(FGBufferFragmentShader, FMaterialShader,
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

	struct FGBufferRenderer::FPipeline
	{
		FMaterialShaderMap ShaderMap;
		std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding> Vertex;
		TMaterialShaderRef<FGBufferFragmentShader> Fragment;
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
		TRendererResourceSlotCache<FGBufferPipelineKey, std::unique_ptr<FPipeline>>
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
		const FPipelineRequest& Request) -> FPipeline*
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
				std::string ErrorMessage;
				const bool bInitialized = RendererPrivate::InitializeMaterialShaderMap(
					*VertexType, FragmentType,
					Factory->GetType(),
					RendererPrivate::MaterialMeshPassGBuffer, ShaderKey.Material,
					Coordinator.GetGeneration_RenderThread(), CompiledProgram.get(),
					Options, ShaderMap, ErrorMessage);
				if (!bInitialized)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"GBufferShaderMap",
							std::to_string(ShaderKey.FactoryKey.HashValue),
							std::move(ErrorMessage),
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
							"RHI shader creation returned null.",
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
			TRenderResourceCreateResult<std::unique_ptr<FPipeline>>;
		std::unique_ptr<FPipeline>* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, Shaders, PipelineKey,
			 VertexDeclaration = Request.VertexDeclaration]()
				-> FPipelineResult {
				auto Candidate = std::make_unique<FPipeline>();
				Candidate->ShaderMap = Shaders->ShaderMap;
				Candidate->Vertex = Shaders->Vertex;
				Candidate->Fragment = Shaders->Fragment;
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
					GDynamicRHI->RHICreateGraphicsPipelineState(
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
							"Graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		return Pipeline != nullptr ? Pipeline->get() : nullptr;
	}

	auto FGBufferRenderer::BindPipeline_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPipeline& Pipeline,
		const FVertexParameters& VertexParameters,
		const FFragmentParameters& FragmentParameters) -> bool
	{
		if (Pipeline.PipelineState == nullptr) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline.PipelineState);
		if (!VertexParameters.Binding || VertexParameters.Binding->GetFactoryKey() != Pipeline.FactoryKey
			|| VertexParameters.Binding->GetLayoutKey() != Pipeline.LayoutKey
			|| !Pipeline.Vertex->Bind(CommandList, VertexParameters.Transform, *VertexParameters.Binding)) return false;

		FGBufferFragmentShader::FParameters Parameters;
		Parameters.Material = FragmentParameters.Material;
		Parameters.BaseColorTexture = FragmentParameters.Textures[0];
		Parameters.NormalTexture = FragmentParameters.Textures[1];
		Parameters.MetallicTexture = FragmentParameters.Textures[2];
		Parameters.RoughnessTexture = FragmentParameters.Textures[3];
		Parameters.AmbientOcclusionTexture = FragmentParameters.Textures[4];
		Parameters.EmissiveTexture = FragmentParameters.Textures[5];
		Parameters.OpacityTexture = FragmentParameters.Textures[6];
		Parameters.OpacityMaskTexture = FragmentParameters.Textures[7];
		Parameters.BaseColorSampler = FragmentParameters.Samplers[0];
		Parameters.NormalSampler = FragmentParameters.Samplers[1];
		Parameters.MetallicSampler = FragmentParameters.Samplers[2];
		Parameters.RoughnessSampler = FragmentParameters.Samplers[3];
		Parameters.AmbientOcclusionSampler = FragmentParameters.Samplers[4];
		Parameters.EmissiveSampler = FragmentParameters.Samplers[5];
		Parameters.OpacitySampler = FragmentParameters.Samplers[6];
		Parameters.OpacityMaskSampler = FragmentParameters.Samplers[7];
		SetShaderParameters(CommandList, Pipeline.Fragment, Parameters);
		return true;
	}

	auto FGBufferRenderer::ReleaseResources_RenderThread() -> void
	{
		State->ShaderMaps.Reset();
		State->Pipelines.Reset();
	}
} // namespace Durin
