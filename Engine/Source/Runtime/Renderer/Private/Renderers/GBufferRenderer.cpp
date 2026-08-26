#include "Renderers/GBufferRenderer.h"

#include "Renderers/MeshRendererShared.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "RHI.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FGBufferLocalVertexShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferLocalVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FGBufferLocalVertexShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGBufferSplineVertexShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferSplineVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(SplineMesh);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FGBufferSplineVertexShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGBufferSkeletalVertexShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferSkeletalVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(SkinPalette);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FGBufferSkeletalVertexShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGBufferTerrainVertexShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGBufferTerrainVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(HeightTexture);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Terrain);
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(TerrainPatchOrigins);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FGBufferTerrainVertexShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGBufferFragmentShader final : public FShader
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
			DURIN_DECLARE_SHADER(FGBufferFragmentShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"GeometryFragmentMain");
		};

		struct FGBufferShaderMapKey
		{
			FMaterialShaderMapIdentity Material;
			EGBufferVertexDomain VertexDomain = EGBufferVertexDomain::Local;
			auto operator==(const FGBufferShaderMapKey&) const -> bool = default;
		};

		struct FGBufferPipelineKey
		{
			FMaterialPlanningPassIdentity Material;
			FRHIRasterizerState Rasterizer;
			FRHIDepthStencilState Depth;
			FVertexDeclarationRHIRef VertexDeclaration;
			EGBufferVertexDomain VertexDomain = EGBufferVertexDomain::Local;
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
				static_cast<uint8>(Key.VertexDomain),
				reinterpret_cast<uintptr_t>(
					Key.VertexDeclaration.GetReference()));
		}
	} // namespace

	struct FGBufferRenderer::FPipeline
	{
		std::shared_ptr<FShaderMapBase> ShaderMap;
		TShaderRef<FGBufferLocalVertexShader> LocalVertex;
		TShaderRef<FGBufferSplineVertexShader> SplineVertex;
		TShaderRef<FGBufferSkeletalVertexShader> SkeletalVertex;
		TShaderRef<FGBufferTerrainVertexShader> TerrainVertex;
		TShaderRef<FGBufferFragmentShader> Fragment;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
		EGBufferVertexDomain VertexDomain = EGBufferVertexDomain::Local;
	};

	struct FGBufferRenderer::FState
	{
		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FGBufferLocalVertexShader> LocalVertex;
			TShaderRef<FGBufferSplineVertexShader> SplineVertex;
			TShaderRef<FGBufferSkeletalVertexShader> SkeletalVertex;
			TShaderRef<FGBufferTerrainVertexShader> TerrainVertex;
			TShaderRef<FGBufferFragmentShader> Fragment;
		};

		TRendererResourceSlotCache<FGBufferShaderMapKey, FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FGBufferPipelineKey, std::unique_ptr<FPipeline>>
			Pipelines{ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FGBufferRenderer::FGBufferRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FRendererTransientTargetPool& InTransientTargets)
		: Coordinator(InCoordinator)
		, TransientTargets(InTransientTargets)
		, State(std::make_unique<FState>())
	{
	}

	FGBufferRenderer::~FGBufferRenderer() = default;

	auto FGBufferRenderer::EnsureTargets_RenderThread(
		uint32 Width,
		uint32 Height) -> std::optional<FTargets>
	{
		if (Width == 0 || Height == 0) return std::nullopt;
		auto MakeDesc = [Width, Height](const char* Name, EPixelFormat Format) {
			return FRHITextureCreateDesc::Create2D(Name, Width, Height, Format)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy)
				.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f));
		};
		const std::array Descriptions{
			MakeDesc("GBufferMaterial", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferNormals", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferSurface", EPixelFormat::RGBA8_UNORM),
			MakeDesc("GBufferEmissive", EPixelFormat::R11G11B10_FLOAT)};
		auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::GBuffer, Descriptions,
			MaximumRetainedBytes);
		if (!Lease) return std::nullopt;
		return FTargets{
			.Material = Lease->Textures[0],
			.Normals = Lease->Textures[1],
			.Surface = Lease->Textures[2],
			.Emissive = Lease->Textures[3]};
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

		const FGBufferShaderMapKey ShaderKey{
			.Material = Request.Material.ShaderMap,
			.VertexDomain = Request.VertexDomain};
		auto& ShaderEntry = State->ShaderMaps.FindOrAdd(ShaderKey);
		using FShaderResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		FState::FShaderMapPayload* Shaders = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, ShaderKey, CompiledProgram = Request.CompiledProgram]() -> FShaderResult {
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
				switch (ShaderKey.VertexDomain)
				{
				case EGBufferVertexDomain::Spline:
					Options.Macros.emplace_back("DURIN_SPLINE_MESH", "1");
					break;
				case EGBufferVertexDomain::Skeletal:
					Options.Macros.emplace_back("DURIN_SKELETAL_MESH", "1");
					break;
				case EGBufferVertexDomain::Terrain:
					Options.Macros.emplace_back("DURIN_TERRAIN", "1");
					break;
				case EGBufferVertexDomain::Local:
				default:
					break;
				}

				FShaderType* VertexType = nullptr;
				switch (ShaderKey.VertexDomain)
				{
				case EGBufferVertexDomain::Spline:
					VertexType = &FGBufferSplineVertexShader::StaticType();
					break;
				case EGBufferVertexDomain::Skeletal:
					VertexType = &FGBufferSkeletalVertexShader::StaticType();
					break;
				case EGBufferVertexDomain::Terrain:
					VertexType = &FGBufferTerrainVertexShader::StaticType();
					break;
				case EGBufferVertexDomain::Local:
				default:
					VertexType = &FGBufferLocalVertexShader::StaticType();
					break;
				}
				FShaderType& FragmentType =
					FGBufferFragmentShader::StaticType();
				std::shared_ptr<FShaderMapBase> ShaderMap;
				std::string ErrorMessage;
				bool bInitialized = false;
				if (CompiledProgram)
				{
					bInitialized = RendererPrivate::InitializeCompiledMaterialShaderMap(
						*VertexType, FragmentType, *CompiledProgram,
						"GeometryFragmentMain", Options, ShaderMap,
						ErrorMessage);
				}
				else
				{
					ShaderMap = std::make_shared<FShaderMapBase>();
					const std::array<const FShaderType*, 2> ShaderTypes{
						VertexType, &FragmentType};
					bInitialized = ShaderMap->InitializeFromShaderTypes(
						ShaderTypes, Options, ErrorMessage);
				}
				if (!bInitialized)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"GBufferShaderMap",
							std::to_string(static_cast<uint8>(
								ShaderKey.VertexDomain)),
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				FShader* Vertex = ShaderMap->GetShader(VertexType);
				auto* Fragment = static_cast<FGBufferFragmentShader*>(
					ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"GBufferShaderMap",
							std::to_string(static_cast<uint8>(
								ShaderKey.VertexDomain)),
							"Compiled map omitted a typed geometry shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}

				FState::FShaderMapPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				switch (ShaderKey.VertexDomain)
				{
				case EGBufferVertexDomain::Spline:
					Candidate.SplineVertex =
						TShaderRef<FGBufferSplineVertexShader>(
							static_cast<FGBufferSplineVertexShader*>(Vertex),
							Candidate.ShaderMap.get());
					break;
				case EGBufferVertexDomain::Skeletal:
					Candidate.SkeletalVertex =
						TShaderRef<FGBufferSkeletalVertexShader>(
							static_cast<FGBufferSkeletalVertexShader*>(Vertex),
							Candidate.ShaderMap.get());
					break;
				case EGBufferVertexDomain::Terrain:
					Candidate.TerrainVertex =
						TShaderRef<FGBufferTerrainVertexShader>(
							static_cast<FGBufferTerrainVertexShader*>(Vertex),
							Candidate.ShaderMap.get());
					break;
				case EGBufferVertexDomain::Local:
				default:
					Candidate.LocalVertex =
						TShaderRef<FGBufferLocalVertexShader>(
							static_cast<FGBufferLocalVertexShader*>(Vertex),
							Candidate.ShaderMap.get());
					break;
				}
				Candidate.Fragment = TShaderRef<FGBufferFragmentShader>(
					Fragment, Candidate.ShaderMap.get());
				FRHIShader* VertexRHI = nullptr;
				switch (ShaderKey.VertexDomain)
				{
				case EGBufferVertexDomain::Spline:
					VertexRHI = Candidate.SplineVertex.GetRHIShader(false);
					break;
				case EGBufferVertexDomain::Skeletal:
					VertexRHI = Candidate.SkeletalVertex.GetRHIShader(false);
					break;
				case EGBufferVertexDomain::Terrain:
					VertexRHI = Candidate.TerrainVertex.GetRHIShader(false);
					break;
				case EGBufferVertexDomain::Local:
				default:
					VertexRHI = Candidate.LocalVertex.GetRHIShader(false);
					break;
				}
				if (VertexRHI == nullptr
					|| Candidate.Fragment.GetRHIShader(false) == nullptr)
				{
					return FShaderResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"GBufferShaderMap",
							std::to_string(static_cast<uint8>(
								ShaderKey.VertexDomain)),
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
			.VertexDomain = Request.VertexDomain};
		auto& PipelineEntry = State->Pipelines.FindOrAdd(PipelineKey);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderEntry.Slot.GetPayloadGeneration().Shader;
		using FPipelineResult =
			TRenderResourceCreateResult<std::unique_ptr<FPipeline>>;
		std::unique_ptr<FPipeline>* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, Shaders, PipelineKey,
			 VertexDeclaration = Request.VertexDeclaration]()
				-> FPipelineResult {
				auto Candidate = std::make_unique<FPipeline>();
				Candidate->ShaderMap = Shaders->ShaderMap;
				Candidate->LocalVertex = Shaders->LocalVertex;
				Candidate->SplineVertex = Shaders->SplineVertex;
				Candidate->SkeletalVertex = Shaders->SkeletalVertex;
				Candidate->TerrainVertex = Shaders->TerrainVertex;
				Candidate->Fragment = Shaders->Fragment;
				Candidate->VertexDeclaration = VertexDeclaration;
				Candidate->VertexDomain = PipelineKey.VertexDomain;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferTargets();
				switch (PipelineKey.VertexDomain)
				{
				case EGBufferVertexDomain::Spline:
					Initializer.BoundShaders.VertexShader =
						Candidate->SplineVertex.GetRHIShader();
					break;
				case EGBufferVertexDomain::Skeletal:
					Initializer.BoundShaders.VertexShader =
						Candidate->SkeletalVertex.GetRHIShader();
					break;
				case EGBufferVertexDomain::Terrain:
					Initializer.BoundShaders.VertexShader =
						Candidate->TerrainVertex.GetRHIShader();
					break;
				case EGBufferVertexDomain::Local:
				default:
					Initializer.BoundShaders.VertexShader =
						Candidate->LocalVertex.GetRHIShader();
					break;
				}
				Initializer.BoundShaders.FragmentShader =
					Candidate->Fragment.GetRHIShader();
				Initializer.VertexDeclaration = VertexDeclaration;
				Initializer.RasterizerState = PipelineKey.Rasterizer;
				Initializer.DepthStencilState = PipelineKey.Depth;
				Initializer.PipelineLayout =
					Candidate->ShaderMap->GetMergedPipelineLayout();
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
		switch (Pipeline.VertexDomain)
		{
		case EGBufferVertexDomain::Spline:
		{
			FGBufferSplineVertexShader::FParameters Parameters;
			Parameters.Transform = VertexParameters.Transform;
			Parameters.SplineMesh = VertexParameters.SplineMesh;
			SetShaderParameters(CommandList, Pipeline.SplineVertex, Parameters);
			break;
		}
		case EGBufferVertexDomain::Skeletal:
		{
			FGBufferSkeletalVertexShader::FParameters Parameters;
			Parameters.Transform = VertexParameters.Transform;
			Parameters.SkinPalette = VertexParameters.SkinPalette;
			SetShaderParameters(CommandList, Pipeline.SkeletalVertex, Parameters);
			break;
		}
		case EGBufferVertexDomain::Terrain:
		{
			FGBufferTerrainVertexShader::FParameters Parameters;
			Parameters.Transform = VertexParameters.Transform;
			Parameters.HeightTexture = VertexParameters.HeightTexture;
			Parameters.Terrain = VertexParameters.Terrain;
			Parameters.TerrainPatchOrigins =
				VertexParameters.TerrainPatchOrigins;
			SetShaderParameters(CommandList, Pipeline.TerrainVertex, Parameters);
			break;
		}
		case EGBufferVertexDomain::Local:
		default:
		{
			FGBufferLocalVertexShader::FParameters Parameters;
			Parameters.Transform = VertexParameters.Transform;
			SetShaderParameters(CommandList, Pipeline.LocalVertex, Parameters);
			break;
		}
		}

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
