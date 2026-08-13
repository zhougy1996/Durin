#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "IScene.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>

namespace Durin
{
	namespace
	{
		class FStaticMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshVertexShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Vertex,
				"VertexMain"
			);
		};

		class FSplineMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSplineMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(SplineMesh);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FSplineMeshVertexShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Vertex, "VertexMain");
		};

		class FStaticMeshFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(BaseColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(NormalTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(MetallicTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(RoughnessTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(AmbientOcclusionTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(EmissiveTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(NormalSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(MetallicSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(RoughnessSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(AmbientOcclusionSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(EmissiveSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacitySampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER(EnvironmentSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(DirectionalShadowTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(DirectionalShadowSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshFragmentShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Fragment,
				"FragmentMain"
			);
		};

		class FStaticMeshOpaqueShadowFragmentShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FStaticMeshOpaqueShadowFragmentShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Fragment,
				"OpaqueShadowFragmentMain"
			);
		};

		class FStaticMeshShadowFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshShadowFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshShadowFragmentShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Fragment,
				"ShadowFragmentMain"
			);
		};

		class FSkeletalMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkeletalMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(SkinPalette);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FSkeletalMeshVertexShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Vertex,
				"VertexMain"
			);
		};

		struct FStaticMeshTransformUniform
		{
			FMatrix4f LocalToClip{1.0f};
			FMatrix4f LocalToWorld{1.0f};
			FMatrix4f NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FSplineMeshUniform
		{
			FVector4f StartPosition{0.0f};
			FVector4f StartTangent{0.0f};
			FVector4f EndPosition{0.0f};
			FVector4f EndTangent{0.0f};
			FVector4f StartEndScale{1.0f};
			FVector4f StartEndOffset{0.0f};
			FVector4f RollUpAxis{0.0f};
			FVector4f SourceRangePolicy{0.0f};
		};

		auto MakeSplineMeshUniform(const FSplineMeshParams& Params) -> FSplineMeshUniform
		{
			FSplineMeshUniform Result;
			Result.StartPosition = FVector4f(FVector3f(Params.StartPosition), static_cast<float>(Params.StartRollRadians));
			Result.StartTangent = FVector4f(FVector3f(Params.StartTangent), static_cast<float>(Params.EndRollRadians));
			Result.EndPosition = FVector4f(FVector3f(Params.EndPosition), 0.0f);
			Result.EndTangent = FVector4f(FVector3f(Params.EndTangent), 0.0f);
			Result.StartEndScale = FVector4f(FVector2f(Params.StartScale), FVector2f(Params.EndScale));
			Result.StartEndOffset = FVector4f(FVector2f(Params.StartOffset), FVector2f(Params.EndOffset));
			Result.RollUpAxis = FVector4f(FVector3f(Params.SplineUpDirection), static_cast<float>(Params.ForwardAxis));
			Result.SourceRangePolicy = FVector4f(static_cast<float>(Params.SourceForwardMin),
				static_cast<float>(Params.SourceForwardMax),
				Params.Interpolation == ESplineMeshInterpolation::SmoothStep ? 1.0f : 0.0f, 0.0f);
			return Result;
		}

		struct FStaticMeshMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f EmissiveMetallic{0.0f};
			FVector4f NormalRoughness{0.0f, 0.0f, 1.0f, 0.5f};
			FVector4f SurfaceParams{1.0f, 1.0f, 1.0f, 0.0f};
			std::array<FVector4f, 8> UVTransforms{};
			FVector4f UVChannels0{0.0f};
			FVector4f UVChannels1{0.0f};
			FVector4f UVRotations0{0.0f};
			FVector4f UVRotations1{0.0f};
		};

		auto GetMaterialSamplerKey(const FMaterialSamplerState& State) -> size_t
		{
			return static_cast<size_t>(State.MinFilter) + 6 * (static_cast<size_t>(State.MagFilter) + 2 * (static_cast<size_t>(State.AddressU) + 3 * static_cast<size_t>(State.AddressV)));
		}

		auto ToRHIAddress(EMaterialSamplerAddressMode Address)
			-> ESamplerAddressMode
		{
			switch (Address)
			{
			case EMaterialSamplerAddressMode::MirroredRepeat:
				return ESamplerAddressMode::MirroredRepeat;
			case EMaterialSamplerAddressMode::ClampToEdge:
				return ESamplerAddressMode::ClampToEdge;
			case EMaterialSamplerAddressMode::Repeat:
			default:
				return ESamplerAddressMode::Repeat;
			}
		}

		auto MakeMaterialSamplerDesc(const FMaterialSamplerState& State)
			-> FRHISamplerDesc
		{
			FRHISamplerDesc Result;
			const uint8 Min = static_cast<uint8>(State.MinFilter);
			Result.MinFilter = (Min & 1u) != 0 ? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MagFilter = State.MagFilter == EMaterialSamplerMagFilter::Linear ? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MipmapMode = Min >= 4 ? ESamplerMipmapMode::Linear : ESamplerMipmapMode::Nearest;
			Result.MaxLod = Min < 2 ? 0.0f : 1000.0f;
			Result.AddressU = ToRHIAddress(State.AddressU);
			Result.AddressV = ToRHIAddress(State.AddressV);
			Result.AddressW = ESamplerAddressMode::Repeat;
			return Result;
		}

		auto GetIdentityText(
			const FMaterialShaderMapIdentity& Identity
		) -> std::string
		{
			return std::format(
				"layout-version={},layout-id={},blend={},shading={},mask-bits={}",
				Identity.RenderLayout.Version,
				Identity.RenderLayout.Id.ToString(),
				static_cast<uint8>(Identity.BlendMode),
				static_cast<uint8>(Identity.ShadingModel),
				std::bit_cast<uint32>(Identity.OpacityMaskThreshold)
			);
		}

		auto MakeShadowRasterizerState(const FRHIRasterizerState& Source)
			-> FRHIRasterizerState
		{
			FRHIRasterizerState Result = Source;
			Result.PolygonMode = ERHIPolygonMode::Fill;
			const bool bPreparedBias = Result.bEnableDepthBias;
			Result.bEnableDepthBias = true;
			if (!bPreparedBias)
			{
				Result.DepthBiasConstantFactor =
					DirectionalShadowDepthBiasConstant;
				Result.DepthBiasSlopeFactor =
					DirectionalShadowDepthBiasSlope;
				Result.DepthBiasClamp = DirectionalShadowDepthBiasClamp;
			}
			return Result;
		}

		auto MakeShadowPipelineKey(
			const FEffectiveStaticMeshPipelineKey& Source)
			-> FEffectiveStaticMeshPipelineKey
		{
			FEffectiveStaticMeshPipelineKey Result = Source;
			Result.Rasterizer = MakeShadowRasterizerState(Source.Rasterizer);
			// Bias magnitudes are dynamic draw state and must not create a new
			// renderer pipeline slot for every shadow-volume adjustment.
			Result.Rasterizer.DepthBiasConstantFactor = 0.0f;
			Result.Rasterizer.DepthBiasSlopeFactor = 0.0f;
			Result.Rasterizer.DepthBiasClamp = 0.0f;
			Result.Depth.bEnableTest = true;
			Result.Depth.bEnableWrite = true;
			Result.Depth.CompareOp = ERHIDepthCompareOp::Less;
			Result.ColorBlend = {};
			return Result;
		}

		auto GetIdentityText(
			const FMaterialPipelineIdentity& Identity
		) -> std::string
		{
			return std::format(
				"{},two-sided={},depth-write={}",
				GetIdentityText(Identity.ShaderMap),
				Identity.bTwoSided,
				static_cast<uint8>(Identity.DepthWritePolicy)
			);
		}

		auto GetIdentityText(
			const FEffectiveStaticMeshPipelineKey& Identity
		) -> std::string
		{
			return std::format(
				"{},vertex-domain={},polygon={},cull={},front={},depth-test={},depth-write={},depth-compare={},blend={},color-src={},color-dst={},color-op={},alpha-src={},alpha-dst={},alpha-op={},write-mask={}",
				GetIdentityText(Identity.Material),
				static_cast<uint8>(Identity.VertexDomain),
				static_cast<uint8>(Identity.Rasterizer.PolygonMode),
				static_cast<uint8>(Identity.Rasterizer.CullMode),
				static_cast<uint8>(Identity.Rasterizer.FrontFace),
				Identity.Depth.bEnableTest,
				Identity.Depth.bEnableWrite,
				static_cast<uint8>(Identity.Depth.CompareOp),
				Identity.ColorBlend.bEnable,
				static_cast<uint8>(Identity.ColorBlend.SrcColorFactor),
				static_cast<uint8>(Identity.ColorBlend.DstColorFactor),
				static_cast<uint8>(Identity.ColorBlend.ColorOp),
				static_cast<uint8>(Identity.ColorBlend.SrcAlphaFactor),
				static_cast<uint8>(Identity.ColorBlend.DstAlphaFactor),
				static_cast<uint8>(Identity.ColorBlend.AlphaOp),
				static_cast<uint8>(Identity.ColorBlend.ColorWriteMask)
			);
		}

		auto MakeMeshDrawSortKey(
			EStaticMeshBasePass Pass,
			const FEffectiveStaticMeshPipelineKey& PipelineKey,
			const FMaterialRenderRepresentation& Representation,
			uint32 NumVertices,
			const FVertexDeclarationElementList& Elements,
			const std::array<uint32, 6>& Geometry,
			uint64 PrimitiveId,
			uint32 LODIndex,
			uint32 SectionIndex) -> FStaticMeshDrawSortKey
		{
			FStaticMeshDrawSortKey Result;
			const FMaterialPipelineIdentity& Material = PipelineKey.Material;
			const FMaterialShaderMapIdentity& Shader = Material.ShaderMap;
			const FGuid& LayoutId = Shader.RenderLayout.Id;
			Result.Pipeline = {
				static_cast<uint32>(Pass), Shader.RenderLayout.Version,
				LayoutId.A, LayoutId.B, LayoutId.C, LayoutId.D,
				static_cast<uint32>(Shader.BlendMode),
				static_cast<uint32>(Shader.ShadingModel),
				std::bit_cast<uint32>(Shader.OpacityMaskThreshold),
				Material.bTwoSided ? 1u : 0u,
				static_cast<uint32>(Material.DepthWritePolicy),
				static_cast<uint32>(PipelineKey.VertexDomain),
				static_cast<uint32>(PipelineKey.Rasterizer.PolygonMode),
				static_cast<uint32>(PipelineKey.Rasterizer.CullMode),
				static_cast<uint32>(PipelineKey.Rasterizer.FrontFace),
				PipelineKey.Depth.bEnableTest ? 1u : 0u,
				PipelineKey.Depth.bEnableWrite ? 1u : 0u,
				static_cast<uint32>(PipelineKey.Depth.CompareOp),
				PipelineKey.ColorBlend.bEnable ? 1u : 0u,
				static_cast<uint32>(PipelineKey.ColorBlend.SrcColorFactor),
				static_cast<uint32>(PipelineKey.ColorBlend.DstColorFactor),
				static_cast<uint32>(PipelineKey.ColorBlend.ColorOp),
				static_cast<uint32>(PipelineKey.ColorBlend.SrcAlphaFactor),
				static_cast<uint32>(PipelineKey.ColorBlend.DstAlphaFactor),
				static_cast<uint32>(PipelineKey.ColorBlend.AlphaOp),
				static_cast<uint32>(PipelineKey.ColorBlend.ColorWriteMask)};
			const std::span<const std::byte> UniformPayload =
				Representation.GetUniformPayload();
			Result.MaterialUniform.reserve(UniformPayload.size());
			for (const std::byte Byte : UniformPayload)
			{
				Result.MaterialUniform.push_back(std::to_integer<uint8>(Byte));
			}

			if (NumVertices != 0)
			{
				Result.VertexFactory[0] = NumVertices;
				for (size_t Index = 0; Index < Elements.size(); ++Index)
				{
					const FVertexElement& Element = Elements[Index];
					const size_t Base = 1 + Index * 5;
					Result.VertexFactory[Base] = Element.StreamIndex;
					Result.VertexFactory[Base + 1] = Element.Offset;
					Result.VertexFactory[Base + 2] =
						static_cast<uint32>(Element.Type);
					Result.VertexFactory[Base + 3] = Element.AttributeIndex;
					Result.VertexFactory[Base + 4] = Element.Stride;
				}
			}
			Result.Geometry = Geometry;
			Result.PrimitiveId = PrimitiveId;
			Result.SelectedLODIndex = LODIndex;
			Result.SectionIndex = SectionIndex;
			return Result;
		}

		auto MakeStaticMeshDrawSortKey(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Draw) -> FStaticMeshDrawSortKey
		{
			const auto Elements = Primitive.VertexFactory != nullptr
				? Primitive.VertexFactory->GetDeclarationElements()
				: FVertexDeclarationElementList{};
			const std::array<uint32, 6> Geometry = Draw.Section != nullptr
				? std::array<uint32, 6>{Draw.Section->FirstIndex,
					Draw.Section->IndexCount, Draw.Section->MinVertexIndex,
					Draw.Section->MaxVertexIndex, Draw.Section->MaterialSlotIndex,
					static_cast<uint32>(Primitive.LOD->IndexBuffer.GetIndices().size())}
				: std::array<uint32, 6>{};
			return MakeMeshDrawSortKey(Draw.Pass, Draw.PipelineKey,
				Draw.Material.Representation, Primitive.VertexFactory != nullptr
					? Primitive.VertexFactory->GetData().NumVertices : 0u,
				Elements, Geometry, Primitive.PrimitiveId.Value,
				Primitive.SelectedLODIndex, Draw.SectionIndex);
		}

		template <typename T>
		auto CompareArray(const T& A, const T& B) -> int
		{
			if (std::ranges::lexicographical_compare(A, B))
			{
				return -1;
			}
			if (std::ranges::lexicographical_compare(B, A))
			{
				return 1;
			}
			return 0;
		}

		auto CompareStaticMeshDrawSortKeys(
			const FStaticMeshDrawSortKey& A,
			const FStaticMeshDrawSortKey& B) -> int
		{
			if (const int Pipeline = CompareArray(A.Pipeline, B.Pipeline);
				Pipeline != 0)
			{
				return Pipeline;
			}
			if (const int Material =
					CompareArray(A.MaterialUniform, B.MaterialUniform);
				Material != 0)
			{
				return Material;
			}
			if (const int VertexFactory =
					CompareArray(A.VertexFactory, B.VertexFactory);
				VertexFactory != 0)
			{
				return VertexFactory;
			}
			if (const int Geometry = CompareArray(A.Geometry, B.Geometry);
				Geometry != 0)
			{
				return Geometry;
			}
			if (A.PrimitiveId != B.PrimitiveId)
			{
				return A.PrimitiveId < B.PrimitiveId ? -1 : 1;
			}
			if (A.SelectedLODIndex != B.SelectedLODIndex)
			{
				return A.SelectedLODIndex < B.SelectedLODIndex ? -1 : 1;
			}
			if (A.SectionIndex != B.SectionIndex)
			{
				return A.SectionIndex < B.SectionIndex ? -1 : 1;
			}
			return 0;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> FMatrix4f
		{
			FMatrix4f Result(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
			{
				for (uint32 Row = 0; Row < 4; ++Row)
				{
					Result[Column][Row] = static_cast<float>(Matrix[Row][Column]);
				}
			}
			return Result;
		}

		auto MakeSkeletalMeshDrawSortKey(
			const FPreparedSkeletalMeshPrimitive& Primitive,
			const FPreparedSkeletalMeshDraw& Draw) -> FStaticMeshDrawSortKey
		{
			const auto Elements = Primitive.VertexFactory != nullptr
				? Primitive.VertexFactory->GetDeclarationElements()
				: FVertexDeclarationElementList{};
			const std::array<uint32, 6> Geometry = Draw.Section != nullptr
				&& Primitive.RenderData != nullptr
				? std::array<uint32, 6>{Draw.Section->FirstIndex,
					Draw.Section->IndexCount, Draw.Section->MinVertexIndex,
					Draw.Section->MaxVertexIndex, Draw.Section->MaterialSlotIndex,
					static_cast<uint32>(Primitive.RenderData->IndexBuffer
						.GetIndices().size())}
				: std::array<uint32, 6>{};
			return MakeMeshDrawSortKey(Draw.Pass, Draw.PipelineKey,
				Draw.Material.Representation, Primitive.VertexFactory != nullptr
					? Primitive.VertexFactory->GetData().NumVertices : 0u,
				Elements, Geometry, Primitive.PrimitiveId.Value, 0,
				Draw.SectionIndex);
		}
	} // namespace

	struct FStaticMeshRenderer::FState
	{
		struct FBaseResources
		{
			std::unordered_map<
				size_t,
				TRenderResourceCreationSlot<FSamplerRHIRef>>
				MaterialSamplerCache;
		};

		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
		};

		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		TRenderResourceCreationSlot<FBaseResources> BaseResources{
			ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FEffectiveStaticMeshPipelineKey,
			FPipelinePayload>
			Pipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
			};
		TRendererResourceSlotCache<
			FEffectiveStaticMeshPipelineKey,
			FPipelinePayload>
			ShadowPipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	struct FSkeletalMeshRenderer::FState
	{
		struct FBaseResources
		{
			std::unordered_map<size_t,
				TRenderResourceCreationSlot<FSamplerRHIRef>> MaterialSamplerCache;
		};
		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
		};
		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FBaseResources> BaseResources{
			ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey,
			FPipelinePayload> Pipelines{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey,
			FPipelinePayload> ShadowPipelines{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode
		,
		std::span<const FPrimitiveSceneInfo* const> SplineSceneInfos
	) -> FPreparedStaticMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(),
			"StaticMesh preparation must occur before the scene render pass.");
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(SceneInfos.size() + SplineSceneInfos.size());
		std::vector<const FPrimitiveSceneInfo*> CombinedSceneInfos;
		CombinedSceneInfos.reserve(SceneInfos.size() + SplineSceneInfos.size());
		CombinedSceneInfos.insert(CombinedSceneInfos.end(), SceneInfos.begin(), SceneInfos.end());
		CombinedSceneInfos.insert(CombinedSceneInfos.end(), SplineSceneInfos.begin(), SplineSceneInfos.end());
		for (const FPrimitiveSceneInfo* SceneInfo : CombinedSceneInfos)
		{
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				continue;
			}
			const bool bSplineMesh = SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SplineMesh;
			++Result.VisibleCandidates;
			if (bSplineMesh) ++Result.VisibleSplineCandidates;
			else ++Result.VisibleLocalCandidates;
			check(bSplineMesh || SceneInfo->GetKind() == EPrimitiveSceneProxyKind::StaticMesh);
			const FStaticMeshSceneProxy* StaticProxy = bSplineMesh ? nullptr : &SceneInfo->GetStaticMeshProxy();
			const FSplineMeshSceneProxy* SplineProxy = bSplineMesh ? &SceneInfo->GetSplineMeshProxy() : nullptr;
			const FStaticMeshRenderData* RenderData = bSplineMesh
				? SplineProxy->GetRenderData() : StaticProxy->GetRenderData();
			if (RenderData == nullptr || RenderData->LODResources.empty()
				|| RenderData->LODVertexFactories.size()
					!= RenderData->LODResources.size())
			{
				++Result.RejectedPrimitives;
				continue;
			}

			std::vector<float> ScreenSizes;
			std::vector<uint8> ReadyLODs;
			ScreenSizes.reserve(RenderData->LODResources.size());
			ReadyLODs.reserve(RenderData->LODResources.size());
			for (uint32 LODIndex = 0;
				 LODIndex < static_cast<uint32>(RenderData->LODResources.size());
				 ++LODIndex)
			{
				ScreenSizes.push_back(
					RenderData->LODResources[LODIndex].ScreenSize);
				ReadyLODs.push_back(
					RenderData->IsReadyForRendering(LODIndex) ? 1u : 0u);
			}
			const FProjectedScreenSizeResult ProjectedSize =
				ComputeProjectedScreenSize(View, SceneInfo->GetWorldBounds());
			if (ProjectedSize.Status != EProjectedScreenSizeStatus::Valid)
			{
				++Result.ProjectedSizeFallbacks;
			}
			const uint32 RequestedLODIndex =
				View.Settings.LODMode == EViewLODMode::ForceLOD0 ? 0u
				: SelectStaticMeshLOD(
					ProjectedSize.NormalizedScreenSize, ScreenSizes);
			const uint32 SelectedLODIndex = ResolveAvailableStaticMeshLOD(
				RequestedLODIndex, ReadyLODs);
			if (SelectedLODIndex == InvalidStaticMeshLODIndex)
			{
				++Result.RejectedPrimitives;
				continue;
			}
			if (SelectedLODIndex != RequestedLODIndex)
			{
				++Result.ResourceFallbacks;
			}
			const FStaticMeshLODResources& LOD =
				RenderData->LODResources[SelectedLODIndex];
			const FLocalVertexFactory& VertexFactory =
				RenderData->LODVertexFactories[SelectedLODIndex].VertexFactory;
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			const FMatrix& LocalToWorld = SceneInfo->GetTransform();
			if (!Math::IsFinite(LocalToWorld))
			{
				++Result.RejectedPrimitives;
				continue;
			}
			const double Determinant = glm::determinant(glm::mat3(LocalToWorld));
			if (!std::isfinite(Determinant))
			{
				++Result.RejectedPrimitives;
				continue;
			}

			const uint32 PrimitiveIndex =
				static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.push_back({
				.PrimitiveId = SceneInfo->GetId(),
				.RequestedLODIndex = RequestedLODIndex,
				.SelectedLODIndex = SelectedLODIndex,
				.LOD = &LOD,
				.VertexFactory = &VertexFactory,
				.VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local,
				.SplineDynamicData = bSplineMesh ? SplineProxy->GetDynamicData() : FSplineMeshRenderDynamicData{},
				.LocalToWorld = LocalToWorld});
			const size_t FirstSectionCount = Result.GetNumSections();
			const size_t FirstTriangleCount = Result.SelectedTriangles;

			for (uint32 SectionIndex = 0;
				 SectionIndex < static_cast<uint32>(LOD.Sections.size());
				 ++SectionIndex)
			{
				const FStaticMeshSection& Section = LOD.Sections[SectionIndex];
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex) + Section.IndexCount
						   > Indices.size())
				{
					continue;
				}

				const FMaterialRenderData& ResolvedMaterial = bSplineMesh
					? SplineProxy->ResolveMaterialRenderData_RenderThread(Section.MaterialSlotIndex)
					: StaticProxy->ResolveMaterialRenderData_RenderThread(Section.MaterialSlotIndex);
				FPreparedStaticMeshDraw Item;
				Item.Material = ResolvedMaterial;
				FMaterialRenderValidationDiagnostic BindingDiagnostic;
				bool bBindingValid = TryGetMaterialRenderV3Binding(
					Item.Material.Representation,
					Item.MaterialBinding,
					BindingDiagnostic
				);
				if (!bBindingValid
					&& Item.Material.Representation.GetLayout().Identity.Version == 2)
				{
					FMaterialRenderV2Binding LegacyBinding;
					bBindingValid = TryGetMaterialRenderV2Binding(
						Item.Material.Representation,
						LegacyBinding,
						BindingDiagnostic
					);
					if (bBindingValid)
					{
						static_cast<FMaterialRenderV2Binding&>(Item.MaterialBinding) =
							std::move(LegacyBinding);
					}
				}
				if (!bBindingValid)
				{
					RecordMaterialFallbackReason(
						EMaterialFallbackReason::UnsupportedLayout
					);
					FRenderResourceCreateDiagnostic Diagnostic;
					Diagnostic.Error = MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"StaticMeshMaterialBinding",
						GetIdentityText(Item.Material.PipelineIdentity.ShaderMap),
						std::format("{} ErrorMaterial was selected.", BindingDiagnostic.Message),
						ERenderResourceGenerationDependency::Manual
					);
					ReportRendererResourceCreateDiagnostic(Diagnostic);
					Item.Material = GetErrorMaterialRenderData();
					FMaterialRenderValidationDiagnostic ErrorDiagnostic;
					if (!TryGetMaterialRenderV3Binding(
							Item.Material.Representation,
							Item.MaterialBinding,
							ErrorDiagnostic
						))
					{
						checkf(false, "ErrorMaterial must satisfy the exact v3 binding contract: %s", ErrorDiagnostic.Message.c_str());
						continue;
					}
				}

				Item.PrimitiveIndex = PrimitiveIndex;
				Item.SectionIndex = SectionIndex;
				Item.Section = &Section;
				Item.ShaderMapIdentity = Item.Material.PipelineIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PipelineIdentity;
				Item.PipelineKey.VertexDomain = Result.Primitives[PrimitiveIndex].VertexDomain;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PipelineIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				const EMaterialBlendMode BlendMode =
					Item.Material.PipelineIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked ? EStaticMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EStaticMeshBasePass::Translucent :
																																				   EStaticMeshBasePass::Opaque;
				const EMaterialDepthWritePolicy DepthPolicy =
					Item.Material.PipelineIdentity.DepthWritePolicy;
				Item.PipelineKey.Depth.bEnableWrite =
					DepthPolicy == EMaterialDepthWritePolicy::Enabled
					|| (DepthPolicy == EMaterialDepthWritePolicy::Automatic
						&& Item.Pass != EStaticMeshBasePass::Translucent);
				if (Item.Pass == EStaticMeshBasePass::Translucent)
				{
					Item.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
				}

				auto TryCenter = [&](const FBox& Bounds, bool bLocal) -> bool {
					if (!Bounds.bIsValid || !Math::IsFinite(Bounds.Min)
						|| !Math::IsFinite(Bounds.Max))
					{
						return false;
					}
					const FVector4 Candidate = bLocal ? LocalToWorld * FVector4(Bounds.GetCenter(), 1.0) : FVector4(Bounds.GetCenter(), 1.0);
					if (!Math::IsFinite(Candidate))
					{
						return false;
					}
					Item.SortCenter = FVector3(Candidate);
					return true;
				};
				if (!TryCenter(Section.LocalBounds, true)
					&& !TryCenter(SceneInfo->GetWorldBounds(), false))
				{
					const FVector4 Origin = LocalToWorld * FVector4(0.0, 0.0, 0.0, 1.0);
					if (!Math::IsFinite(Origin))
					{
						continue;
					}
					Item.SortCenter = FVector3(Origin);
				}
				const FVector3 Offset = Item.SortCenter - View.ViewLocation;
				Item.TranslucentDistanceSquared = glm::dot(Offset, Offset);
				if (!std::isfinite(Item.TranslucentDistanceSquared))
				{
					continue;
				}
				const bool bFiniteSortKey = std::isfinite(
					Item.PipelineKey.Material.ShaderMap.OpacityMaskThreshold);
				checkf(bFiniteSortKey,
					"StaticMesh prepared ordering keys must be finite.");
				if (!bFiniteSortKey)
				{
					continue;
				}
				Item.SortKey = MakeStaticMeshDrawSortKey(
					Result.Primitives[PrimitiveIndex], Item);

				switch (Item.Pass)
				{
				case EStaticMeshBasePass::Opaque:
					++Result.OpaqueSections;
					Result.OpaqueTriangles += Section.IndexCount / 3;
					Result.Opaque.push_back(std::move(Item));
					break;
				case EStaticMeshBasePass::Masked:
					++Result.MaskedSections;
					Result.MaskedTriangles += Section.IndexCount / 3;
					Result.Masked.push_back(std::move(Item));
					break;
				case EStaticMeshBasePass::Translucent:
					++Result.TranslucentSections;
					Result.TranslucentTriangles += Section.IndexCount / 3;
					Result.Translucent.push_back(std::move(Item));
					break;
				}
				Result.SelectedTriangles += Section.IndexCount / 3;
			}

			const size_t PreparedSectionCount =
				Result.GetNumSections() - FirstSectionCount;
			if (PreparedSectionCount == 0)
			{
				Result.Primitives.pop_back();
				Result.SelectedTriangles = FirstTriangleCount;
				++Result.RejectedPrimitives;
				continue;
			}
			if (bSplineMesh)
			{
				++Result.PreparedSplinePrimitives;
				Result.PreparedSplineSections += PreparedSectionCount;
				Result.PreparedSplineTriangles += Result.SelectedTriangles - FirstTriangleCount;
				Result.RetainedSplineDeformationBytes += sizeof(FSplineMeshRenderDynamicData);
				Result.AcceptedSplineDynamicUpdates += SplineProxy->GetAcceptedDynamicUpdateCount();
			}
			else ++Result.PreparedLocalPrimitives;
			Result.SelectedSections += PreparedSectionCount;
			const size_t HistogramSize = RenderData->LODResources.size();
			Result.RequestedLODHistogram.resize(
				std::max(Result.RequestedLODHistogram.size(), HistogramSize));
			Result.SelectedLODHistogram.resize(
				std::max(Result.SelectedLODHistogram.size(), HistogramSize));
			++Result.RequestedLODHistogram[RequestedLODIndex];
			++Result.SelectedLODHistogram[SelectedLODIndex];
		}
		Result.RejectedSplinePrimitives = Result.VisibleSplineCandidates
			- std::min(Result.VisibleSplineCandidates, Result.PreparedSplinePrimitives);
		auto CountInputStateGroups = [](const auto& Bucket) -> size_t {
			if (Bucket.empty())
			{
				return 0;
			}
			size_t Groups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const FStaticMeshDrawSortKey& Previous =
					Bucket[Index - 1].SortKey;
				const FStaticMeshDrawSortKey& Current = Bucket[Index].SortKey;
				const bool bStateChanged = Previous.Pipeline != Current.Pipeline
					|| Previous.MaterialUniform != Current.MaterialUniform
					|| Previous.VertexFactory != Current.VertexFactory;
				Groups += bStateChanged ? 1u : 0u;
			}
			return Groups;
		};
		Result.OpaqueInputStateGroups = CountInputStateGroups(Result.Opaque);
		Result.MaskedInputStateGroups = CountInputStateGroups(Result.Masked);
		auto StateSort = [](const FPreparedStaticMeshDraw& A,
			const FPreparedStaticMeshDraw& B) {
			return CompareStaticMeshDrawSortKeys(A.SortKey, B.SortKey) < 0;
		};
		std::ranges::sort(Result.Opaque, StateSort);
		std::ranges::sort(Result.Masked, StateSort);
		std::ranges::sort(
			Result.Translucent,
			[](const FPreparedStaticMeshDraw& A,
			   const FPreparedStaticMeshDraw& B) {
				if (A.TranslucentDistanceSquared
					!= B.TranslucentDistanceSquared)
				{
					return A.TranslucentDistanceSquared
						> B.TranslucentDistanceSquared;
				}
				return CompareStaticMeshDrawSortKeys(A.SortKey, B.SortKey) < 0;
			}
		);

		auto CountStateFacts = [&Result](const auto& Bucket) -> size_t {
			if (Bucket.empty())
			{
				return 0;
			}
			size_t StateGroups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const FStaticMeshDrawSortKey& Previous = Bucket[Index - 1].SortKey;
				const FStaticMeshDrawSortKey& Current = Bucket[Index].SortKey;
				const bool bPipelineChanged =
					Previous.Pipeline != Current.Pipeline;
				const bool bMaterialChanged =
					Previous.MaterialUniform != Current.MaterialUniform;
				const bool bVertexFactoryChanged =
					Previous.VertexFactory != Current.VertexFactory;
				const bool bGeometryChanged =
					Previous.Geometry != Current.Geometry
					|| Previous.PrimitiveId != Current.PrimitiveId
					|| Previous.SelectedLODIndex != Current.SelectedLODIndex;
				Result.PipelineTransitions += bPipelineChanged ? 1u : 0u;
				Result.MaterialTransitions += bMaterialChanged ? 1u : 0u;
				Result.VertexFactoryTransitions +=
					bVertexFactoryChanged ? 1u : 0u;
				Result.GeometryTransitions += bGeometryChanged ? 1u : 0u;
				StateGroups += bPipelineChanged || bMaterialChanged
					|| bVertexFactoryChanged ? 1u : 0u;
			}
			return StateGroups;
		};
		Result.OpaqueStateGroups = CountStateFacts(Result.Opaque);
		Result.MaskedStateGroups = CountStateFacts(Result.Masked);
		const bool bOpaqueGroupingDidNotRegress =
			Result.OpaqueStateGroups <= Result.OpaqueInputStateGroups;
		const bool bMaskedGroupingDidNotRegress =
			Result.MaskedStateGroups <= Result.MaskedInputStateGroups;
		check(bOpaqueGroupingDidNotRegress);
		check(bMaskedGroupingDidNotRegress);
		const size_t RequestedHistogramTotal = std::accumulate(
			Result.RequestedLODHistogram.begin(),
			Result.RequestedLODHistogram.end(), size_t{0});
		const size_t SelectedHistogramTotal = std::accumulate(
			Result.SelectedLODHistogram.begin(),
			Result.SelectedLODHistogram.end(), size_t{0});
		const size_t PreparedPrimitiveCount = Result.Primitives.size();
		const size_t PreparedDrawCount = Result.GetNumSections();
		const bool bPrimitiveCountersConserved = Result.VisibleCandidates
			== PreparedPrimitiveCount + Result.RejectedPrimitives;
		const bool bSectionCountersConserved =
			Result.SelectedSections == PreparedDrawCount;
		const bool bPassSectionCountersConserved = Result.SelectedSections
			== Result.OpaqueSections + Result.MaskedSections
				+ Result.TranslucentSections;
		const bool bPassTriangleCountersConserved = Result.SelectedTriangles
			== Result.OpaqueTriangles + Result.MaskedTriangles
				+ Result.TranslucentTriangles;
		const bool bRequestedHistogramConserved =
			RequestedHistogramTotal == PreparedPrimitiveCount;
		const bool bSelectedHistogramConserved =
			SelectedHistogramTotal == PreparedPrimitiveCount;
		check(bPrimitiveCountersConserved);
		check(bSectionCountersConserved);
		check(bPassSectionCountersConserved);
		check(bPassTriangleCountersConserved);
		check(bRequestedHistogramConserved);
		check(bSelectedHistogramConserved);
		return Result;
	}

	auto PrepareSkeletalMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode) -> FPreparedSkeletalMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(),
			"SkeletalMesh preparation must occur before the scene render pass.");
		FPreparedSkeletalMeshView Result;
		Result.Primitives.reserve(SceneInfos.size());
		for (const FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			++Result.VisibleCandidates;
			++Result.RequestedPaletteUploads;
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				continue;
			}
			check(SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SkeletalMesh);
			const FSkeletalMeshSceneProxy& Proxy =
				SceneInfo->GetSkeletalMeshProxy();
			const FSkeletalMeshRenderData* RenderData = Proxy.GetRenderData();
			const std::shared_ptr<const FSkeletalPosePalette>& Pose = Proxy.GetPose();
			const FMatrix& LocalToWorld = SceneInfo->GetTransform();
			const bool bPoseComplete = Pose != nullptr && !Pose->Matrices.empty()
				&& RenderData != nullptr
				&& Pose->Matrices.size() == RenderData->PaletteBoneIndices.size()
				&& std::ranges::all_of(Pose->Matrices,
					[](const FMatrix4f& Matrix) { return Math::IsFinite(Matrix); });
			const uint64 PaletteBytes = Pose != nullptr
				? Pose->Matrices.size() * sizeof(FMatrix4f) : 0;
			const FRHICapabilities* Capabilities = GDynamicRHI != nullptr
				? GDynamicRHI->RHIGetCapabilities() : nullptr;
			if (RenderData == nullptr || !RenderData->IsReadyForRendering()
				|| !bPoseComplete || !Math::IsFinite(LocalToWorld)
				|| Capabilities == nullptr
				|| Capabilities->MinStorageBufferOffsetAlignment == 0
				|| PaletteBytes == 0
				|| PaletteBytes > Capabilities->MaxStorageBufferRange
				|| PaletteBytes > MaximumSkeletalPosePaletteBytes)
			{
				++Result.RejectedPrimitives;
				++Result.RejectedPalettes;
				continue;
			}
			const double Determinant = glm::determinant(glm::mat3(LocalToWorld));
			if (!std::isfinite(Determinant))
			{
				++Result.RejectedPrimitives;
				continue;
			}

			const uint32 PrimitiveIndex = static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.push_back({
				.PrimitiveId = SceneInfo->GetId(), .RenderData = RenderData,
				.VertexFactory = &RenderData->VertexFactory, .Pose = Pose,
				.LocalToWorld = LocalToWorld});
			const size_t FirstSectionCount = Result.GetNumSections();
			const size_t FirstTriangleCount = Result.SelectedTriangles;
			const auto& Indices = RenderData->IndexBuffer.GetIndices();
			for (uint32 SectionIndex = 0;
				 SectionIndex < RenderData->Sections.size(); ++SectionIndex)
			{
				const FSkeletalMeshRenderSection& Section =
					RenderData->Sections[SectionIndex];
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex) + Section.IndexCount
						> Indices.size()) continue;
				FPreparedSkeletalMeshDraw Item;
				Item.Material = Proxy.ResolveMaterialRenderData_RenderThread(
					Section.MaterialSlotIndex);
				FMaterialRenderValidationDiagnostic BindingDiagnostic;
				bool bBindingValid = TryGetMaterialRenderV3Binding(
					Item.Material.Representation, Item.MaterialBinding,
					BindingDiagnostic);
				if (!bBindingValid
					&& Item.Material.Representation.GetLayout().Identity.Version == 2)
				{
					FMaterialRenderV2Binding Legacy;
					bBindingValid = TryGetMaterialRenderV2Binding(
						Item.Material.Representation, Legacy, BindingDiagnostic);
					if (bBindingValid)
						static_cast<FMaterialRenderV2Binding&>(Item.MaterialBinding) =
							std::move(Legacy);
				}
				if (!bBindingValid)
				{
					RecordMaterialFallbackReason(EMaterialFallbackReason::UnsupportedLayout);
					Item.Material = GetErrorMaterialRenderData();
					FMaterialRenderValidationDiagnostic ErrorDiagnostic;
					if (!TryGetMaterialRenderV3Binding(Item.Material.Representation,
						Item.MaterialBinding, ErrorDiagnostic)) continue;
				}
				Item.PrimitiveIndex = PrimitiveIndex;
				Item.SectionIndex = SectionIndex;
				Item.Section = &Section;
				Item.ShaderMapIdentity = Item.Material.PipelineIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PipelineIdentity;
				Item.PipelineKey.VertexDomain = EVertexDeformationDomain::Skeletal;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe
					? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PipelineIdentity.bTwoSided
					? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0
					? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				const EMaterialBlendMode BlendMode =
					Item.Material.PipelineIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked
					? EStaticMeshBasePass::Masked
					: BlendMode == EMaterialBlendMode::Translucent
						? EStaticMeshBasePass::Translucent
						: EStaticMeshBasePass::Opaque;
				const auto DepthPolicy = Item.Material.PipelineIdentity.DepthWritePolicy;
				Item.PipelineKey.Depth.bEnableWrite =
					DepthPolicy == EMaterialDepthWritePolicy::Enabled
					|| (DepthPolicy == EMaterialDepthWritePolicy::Automatic
						&& Item.Pass != EStaticMeshBasePass::Translucent);
				if (Item.Pass == EStaticMeshBasePass::Translucent)
					Item.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
				const FVector4 Center = LocalToWorld
					* FVector4(Section.LocalBounds.GetCenter(), 1.0);
				if (!Math::IsFinite(Center)) continue;
				Item.SortCenter = FVector3(Center);
				const FVector3 Offset = Item.SortCenter - View.ViewLocation;
				Item.TranslucentDistanceSquared = glm::dot(Offset, Offset);
				if (!std::isfinite(Item.TranslucentDistanceSquared)) continue;
				Item.bCastsShadow = Item.Pass != EStaticMeshBasePass::Translucent;
				Item.SortKey = MakeSkeletalMeshDrawSortKey(
					Result.Primitives[PrimitiveIndex], Item);
				auto* Bucket = Item.Pass == EStaticMeshBasePass::Opaque
					? &Result.Opaque : Item.Pass == EStaticMeshBasePass::Masked
						? &Result.Masked : &Result.Translucent;
				Bucket->push_back(std::move(Item));
				Result.SelectedTriangles += Section.IndexCount / 3;
				if (BlendMode == EMaterialBlendMode::Masked)
				{
					++Result.MaskedSections;
					Result.MaskedTriangles += Section.IndexCount / 3;
				}
				else if (BlendMode == EMaterialBlendMode::Translucent)
				{
					++Result.TranslucentSections;
					Result.TranslucentTriangles += Section.IndexCount / 3;
				}
				else
				{
					++Result.OpaqueSections;
					Result.OpaqueTriangles += Section.IndexCount / 3;
				}
			}
			const size_t PreparedSections = Result.GetNumSections() - FirstSectionCount;
			if (PreparedSections == 0)
			{
				Result.Primitives.pop_back();
				Result.SelectedTriangles = FirstTriangleCount;
				++Result.RejectedPrimitives;
				continue;
			}
			Result.SelectedSections += PreparedSections;
		}
		auto StateSort = [](const FPreparedSkeletalMeshDraw& A,
			const FPreparedSkeletalMeshDraw& B) {
			return CompareStaticMeshDrawSortKeys(A.SortKey, B.SortKey) < 0;
		};
		std::ranges::sort(Result.Opaque, StateSort);
		std::ranges::sort(Result.Masked, StateSort);
		std::ranges::sort(Result.Translucent,
			[](const FPreparedSkeletalMeshDraw& A,
				const FPreparedSkeletalMeshDraw& B) {
				if (A.TranslucentDistanceSquared != B.TranslucentDistanceSquared)
					return A.TranslucentDistanceSquared > B.TranslucentDistanceSquared;
				return CompareStaticMeshDrawSortKeys(A.SortKey, B.SortKey) < 0;
			});
		auto CountStateFacts = [&Result](const auto& Bucket) -> size_t {
			if (Bucket.empty()) return 0;
			size_t Groups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const auto& Previous = Bucket[Index - 1].SortKey;
				const auto& Current = Bucket[Index].SortKey;
				const bool bPipeline = Previous.Pipeline != Current.Pipeline;
				const bool bMaterial =
					Previous.MaterialUniform != Current.MaterialUniform;
				const bool bVertexFactory =
					Previous.VertexFactory != Current.VertexFactory;
				const bool bGeometry = Previous.Geometry != Current.Geometry
					|| Previous.PrimitiveId != Current.PrimitiveId;
				Result.PipelineTransitions += bPipeline ? 1u : 0u;
				Result.MaterialTransitions += bMaterial ? 1u : 0u;
				Result.VertexFactoryTransitions += bVertexFactory ? 1u : 0u;
				Result.GeometryTransitions += bGeometry ? 1u : 0u;
				Groups += bPipeline || bMaterial || bVertexFactory ? 1u : 0u;
			}
			return Groups;
		};
		Result.OpaqueStateGroups = CountStateFacts(Result.Opaque);
		Result.MaskedStateGroups = CountStateFacts(Result.Masked);
		CountStateFacts(Result.Translucent);
		check(Result.VisibleCandidates
			== Result.Primitives.size() + Result.RejectedPrimitives);
		check(Result.SelectedSections == Result.GetNumSections());
		check(Result.SelectedTriangles == Result.OpaqueTriangles
			+ Result.MaskedTriangles + Result.TranslucentTriangles);
		return Result;
	}

	FStaticMeshRenderer::FStaticMeshRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures,
		FEnvironmentLightingResources& InEnvironmentLighting
	)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, EnvironmentLighting(InEnvironmentLighting)
		, State(std::make_unique<FState>())
	{
	}

	FStaticMeshRenderer::~FStaticMeshRenderer() = default;

	auto FStaticMeshRenderer::EnsureBaseResources_RenderThread() -> bool
	{
		check(IsInRenderingThread());
		using FResult =
			TRenderResourceCreateResult<FState::FBaseResources>;
		return State->BaseResources.Resolve(
				   Coordinator.GetGeneration_RenderThread(),
				   []() -> FResult {
					   return FResult::Success(FState::FBaseResources{});
				   },
				   ReportRendererResourceCreateDiagnostic
			   )
			   != nullptr;
	}

	auto FStaticMeshRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedStaticMeshView& PreparedView) -> bool
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(),
			"StaticMesh resource preparation must occur before the scene render pass.");
		checkf(PreparedView.Phase == EPreparedStaticMeshPhase::Prepared,
			"StaticMesh resources may only be prepared once for their owning view.");
		PreparedView.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		if (!EnsureBaseResources_RenderThread())
		{
			PreparedView.ResourcePreparationRejectedDraws =
				PreparedView.ResourcePreparationAttemptedDraws;
			PreparedView.Phase = EPreparedStaticMeshPhase::ResourcesPrepared;
			return false;
		}
		auto PrepareBucket = [this, &PreparedView](auto& Bucket) {
			for (FPreparedStaticMeshDraw& Item : Bucket)
			{
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Item);
				Item.bResourcesReady = Primitive != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Item);
				PreparedView.ResourcePreparationSuccessfulDraws +=
					Item.bResourcesReady ? 1u : 0u;
			}
		};
		PrepareBucket(PreparedView.Opaque);
		PrepareBucket(PreparedView.Masked);
		PrepareBucket(PreparedView.Translucent);
		PreparedView.ResourcePreparationRejectedDraws =
			PreparedView.ResourcePreparationAttemptedDraws
				- PreparedView.ResourcePreparationSuccessfulDraws;
		PreparedView.Phase = EPreparedStaticMeshPhase::ResourcesPrepared;
		const bool bResourceCountersConserved =
			PreparedView.ResourcePreparationAttemptedDraws
			== PreparedView.ResourcePreparationSuccessfulDraws
				+ PreparedView.ResourcePreparationRejectedDraws;
		check(bResourceCountersConserved);
		return PreparedView.ResourcePreparationRejectedDraws == 0;
	}

	auto FStaticMeshRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedStaticMeshView& PreparedView) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedStaticMeshPhase::Prepared);
		check(PreparedView.Translucent.empty());
		PreparedView.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		if (!EnsureBaseResources_RenderThread())
		{
			PreparedView.ResourcePreparationRejectedDraws =
				PreparedView.ResourcePreparationAttemptedDraws;
			PreparedView.Phase = EPreparedStaticMeshPhase::ResourcesPrepared;
			return false;
		}
		for (auto* Bucket : {&PreparedView.Opaque, &PreparedView.Masked})
		{
			for (FPreparedStaticMeshDraw& Draw : *Bucket)
			{
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				Draw.bResourcesReady = Primitive != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Draw, true);
				PreparedView.ResourcePreparationSuccessfulDraws +=
					Draw.bResourcesReady ? 1u : 0u;
			}
		}
		PreparedView.ResourcePreparationRejectedDraws =
			PreparedView.ResourcePreparationAttemptedDraws
				- PreparedView.ResourcePreparationSuccessfulDraws;
		PreparedView.Phase = EPreparedStaticMeshPhase::ResourcesPrepared;
		return PreparedView.ResourcePreparationRejectedDraws == 0;
	}

	auto FStaticMeshRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		FPreparedStaticMeshView& PreparedView) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedStaticMeshPhase::ResourcesPrepared);
		for (const auto* Bucket : {&PreparedView.Opaque, &PreparedView.Masked})
		{
			for (const FPreparedStaticMeshDraw& Draw : *Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && Draw.bResourcesReady
					&& DrawSection_RenderThread(
						CommandList, ShadowView, FallbackLighting,
						ERenderMode::Unlit, *Primitive, Draw, true))
					++PreparedView.SuccessfulDraws;
				else
					++PreparedView.RejectedDraws;
			}
		}
		PreparedView.Phase = EPreparedStaticMeshPhase::Executed;
		check(PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws);
	}

	auto FStaticMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		bool bShadowDepth) -> bool
	{
		check(IsInRenderingThread());
		FState::FBaseResources* BaseResources =
			State->BaseResources.GetPayload();
		if (BaseResources == nullptr || Primitive.VertexFactory == nullptr)
		{
			return false;
		}
		const FMaterialRenderData& Material = Item.Material;
		const FLocalVertexFactory& VertexFactory = *Primitive.VertexFactory;

		using FShaderMapResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		const FMeshShaderMapKey ShaderMapKey{
			.Material = Material.PipelineIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain};
		auto& ShaderMapCache = bShadowDepth
			? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAdd(ShaderMapKey);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, Domain = Primitive.VertexDomain,
				 bShadowDepth]() -> FShaderMapResult {
					const FMaterialShaderMapIdentity& Identity =
						Material.PipelineIdentity.ShaderMap;
					FShaderCompileOptions CompileOptions;
					CompileOptions.bForceRecompile =
						Coordinator.ShouldForceShaderRecompile_RenderThread();
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_BLEND_MODE",
						std::to_string(static_cast<uint8>(Identity.BlendMode)));
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_SHADING_MODEL",
						std::to_string(static_cast<uint8>(Identity.ShadingModel)));
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
						std::to_string(std::bit_cast<uint32>(
							Identity.OpacityMaskThreshold)));
					if (Domain == EVertexDeformationDomain::Spline)
						CompileOptions.Macros.emplace_back("DURIN_SPLINE_MESH", "1");
					FShaderType& VertexShaderType = Domain == EVertexDeformationDomain::Spline
						? FSplineMeshVertexShader::StaticType() : FStaticMeshVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FStaticMeshFragmentShader::StaticType();
					FShaderType& ShadowFragmentShaderType =
						FStaticMeshShadowFragmentShader::StaticType();
					FShaderType& OpaqueShadowFragmentShaderType =
						FStaticMeshOpaqueShadowFragmentShader::StaticType();
					std::vector<const FShaderType*> ShaderTypes{&VertexShaderType};
					if (!bShadowDepth)
						ShaderTypes.push_back(&FragmentShaderType);
					else if (Identity.BlendMode == EMaterialBlendMode::Masked)
						ShaderTypes.push_back(&ShadowFragmentShaderType);
					else ShaderTypes.push_back(&OpaqueShadowFragmentShaderType);
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderCompile,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								std::move(ErrorMessage),
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual));
					}
					FShader* VertexShader = ShaderMap->GetShader(&VertexShaderType);
					auto* FragmentShader = !bShadowDepth
						? static_cast<FStaticMeshFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType)) : nullptr;
					auto* ShadowFragmentShader =
						bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked
							? static_cast<FStaticMeshShadowFragmentShader*>(
								ShaderMap->GetShader(&ShadowFragmentShaderType))
							: nullptr;
					auto* OpaqueShadowFragmentShader =
						bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
							? static_cast<FStaticMeshOpaqueShadowFragmentShader*>(
								ShaderMap->GetShader(&OpaqueShadowFragmentShaderType))
							: nullptr;
					if (VertexShader == nullptr
						|| (!bShadowDepth && FragmentShader == nullptr))
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"Compiled shader map did not contain both typed shaders.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual));
					if (bShadowDepth
						&& Identity.BlendMode == EMaterialBlendMode::Masked
						&& ShadowFragmentShader == nullptr)
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"Compiled masked shader map did not contain the shadow fragment shader.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual));
					}
					if (bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked
						&& OpaqueShadowFragmentShader == nullptr)
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap", GetIdentityText(Identity),
								"Compiled shader map did not contain the opaque shadow fragment shader.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual));
					FState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					if (Domain == EVertexDeformationDomain::Spline)
						Candidate.SplineVertexShader = TShaderRef<FSplineMeshVertexShader>(
							static_cast<FSplineMeshVertexShader*>(VertexShader), Candidate.ShaderMap.get());
					else Candidate.VertexShader = TShaderRef<FStaticMeshVertexShader>(
						static_cast<FStaticMeshVertexShader*>(VertexShader), Candidate.ShaderMap.get());
					if (FragmentShader != nullptr)
						Candidate.FragmentShader = TShaderRef<FStaticMeshFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					if (ShadowFragmentShader != nullptr)
						Candidate.ShadowFragmentShader =
							TShaderRef<FStaticMeshShadowFragmentShader>(
								ShadowFragmentShader, Candidate.ShaderMap.get());
					if (OpaqueShadowFragmentShader != nullptr)
						Candidate.OpaqueShadowFragmentShader =
							TShaderRef<FStaticMeshOpaqueShadowFragmentShader>(
								OpaqueShadowFragmentShader, Candidate.ShaderMap.get());
					if ((Domain == EVertexDeformationDomain::Spline
						? Candidate.SplineVertexShader.GetRHIShader(false)
						: Candidate.VertexShader.GetRHIShader(false)) == nullptr
						|| (!bShadowDepth
							&& Candidate.FragmentShader.GetRHIShader(false) == nullptr)
						|| (bShadowDepth
							&& Identity.BlendMode == EMaterialBlendMode::Masked
							&& Candidate.ShadowFragmentShader.GetRHIShader(false) == nullptr)
						|| (bShadowDepth
							&& Identity.BlendMode != EMaterialBlendMode::Masked
							&& Candidate.OpaqueShadowFragmentShader.GetRHIShader(false) == nullptr))
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::RHIResource,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"RHI shader creation returned null.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Device
									| ERenderResourceGenerationDependency::Manual));
					}
					return FShaderMapResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic);
		if (ShaderMapPayload == nullptr)
		{
			return false;
		}

		using FPipelineResult =
			TRenderResourceCreateResult<FState::FPipelinePayload>;
		const FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey)
				: Item.PipelineKey;
		auto& PipelineCache = bShadowDepth
			? State->ShadowPipelines : State->Pipelines;
		auto& PipelineEntry = PipelineCache.FindOrAdd(EffectivePipelineKey);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderMapEntry.Slot.GetPayloadGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, ShaderMapPayload, &EffectivePipelineKey,
			 bShadowDepth,
			 &VertexFactory]() -> FPipelineResult {
				const FEffectiveStaticMeshPipelineKey& Identity = EffectivePipelineKey;
				FState::FPipelinePayload Candidate;
				Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
				Candidate.VertexShader = ShaderMapPayload->VertexShader;
				Candidate.SplineVertexShader = ShaderMapPayload->SplineVertexShader;
				Candidate.FragmentShader = ShaderMapPayload->FragmentShader;
				Candidate.ShadowFragmentShader =
					ShaderMapPayload->ShadowFragmentShader;
				Candidate.OpaqueShadowFragmentShader =
					ShaderMapPayload->OpaqueShadowFragmentShader;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = bShadowDepth
					? RenderTargetLayouts::MakeDirectionalShadowDepth()
					: RenderTargetLayouts::MakeSceneTargets();
				Initializer.BoundShaders.VertexShader = Identity.VertexDomain == EVertexDeformationDomain::Spline
					? Candidate.SplineVertexShader.GetRHIShader()
					: Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth
					? (Identity.Material.ShaderMap.BlendMode
							== EMaterialBlendMode::Masked
						? Candidate.ShadowFragmentShader.GetRHIShader()
						: Candidate.OpaqueShadowFragmentShader.GetRHIShader())
					: Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory.GetDeclaration();
				Initializer.RasterizerState = Identity.Rasterizer;
				Initializer.DepthStencilState = Identity.Depth;
				if (!bShadowDepth)
					Initializer.ColorBlendStates[0] = Identity.ColorBlend;
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						FName(std::format(
							"StaticMeshPipeline_{}", PipelineEntry.Index)),
						Initializer);
				if (Candidate.PipelineState == nullptr)
				{
					return FPipelineResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"StaticMeshPipeline",
							GetIdentityText(Identity),
							"Graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Pipeline == nullptr)
		{
			return false;
		}

		for (const FMaterialSamplerState& SamplerState :
			 Item.MaterialBinding.Samplers)
		{
			auto Entry = BaseResources->MaterialSamplerCache.try_emplace(
				GetMaterialSamplerKey(SamplerState),
				ERenderResourceGenerationDependency::Device).first;
			using FSamplerResult = TRenderResourceCreateResult<FSamplerRHIRef>;
			FSamplerRHIRef* Sampler = Entry->second.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[SamplerState]() -> FSamplerResult {
					FSamplerRHIRef Candidate =
						RHICreateSampler(MakeMaterialSamplerDesc(SamplerState));
					if (Candidate == nullptr)
					{
						return FSamplerResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::RHIResource,
								"StaticMeshMaterialSampler",
								std::format("min={},mag={},u={},v={}",
									static_cast<uint8>(SamplerState.MinFilter),
									static_cast<uint8>(SamplerState.MagFilter),
									static_cast<uint8>(SamplerState.AddressU),
									static_cast<uint8>(SamplerState.AddressV)),
								"RHI sampler creation returned null.",
								ERenderResourceGenerationDependency::Device
									| ERenderResourceGenerationDependency::Manual));
					}
					return FSamplerResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic);
			if (Sampler == nullptr)
			{
				return false;
			}
		}
		return true;
	}

	auto FStaticMeshRenderer::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRHIUniformBufferRange& Lighting,
		ERenderMode RenderMode,
		FPreparedStaticMeshView& PreparedView
	) -> void
	{
		check(IsInRenderingThread());
		checkf(CommandList.IsInsideRenderPass(),
			"StaticMesh execution requires the owning scene render pass.");
		checkf(PreparedView.Phase == EPreparedStaticMeshPhase::ResourcesPrepared,
			"StaticMesh execution must remain inside its prepared view lifetime and occur exactly once after resource preparation.");
		if (RenderMode != ERenderMode::Unlit
			&& RenderMode != ERenderMode::Lit)
		{
			PreparedView.Phase = EPreparedStaticMeshPhase::Executed;
			return;
		}
		auto DrawBucket = [&](const auto& Bucket, EStaticMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Item : Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Item);
				const bool bBucketMatches = Item.Pass == Pass;
				const bool bSortKeyMatchesPass = Item.SortKey.Pipeline[0]
					== static_cast<uint32>(Pass);
				checkf(bBucketMatches,
					"StaticMesh prepared bucket does not match its pass.");
				checkf(bSortKeyMatchesPass,
					"StaticMesh prepared sort key does not match its bucket.");
				const bool bComplete = Primitive != nullptr
						&& Primitive->PrimitiveId != InvalidPrimitiveSceneId
						&& Primitive->LOD != nullptr
						&& Primitive->VertexFactory != nullptr
						&& Item.Section != nullptr
						&& std::isfinite(Item.TranslucentDistanceSquared)
						&& Item.ShaderMapIdentity
							== Item.Material.PipelineIdentity.ShaderMap
						&& Item.PipelineKey.Material
							== Item.Material.PipelineIdentity;
				checkf(bComplete,
					"StaticMesh execution requires one complete prepared section.");
				if (!bBucketMatches || !bSortKeyMatchesPass || !bComplete
					|| !Item.bResourcesReady)
				{
					++PreparedView.RejectedDraws;
					continue;
				}
				if (DrawSection_RenderThread(
						CommandList, View, Lighting, RenderMode, *Primitive, Item))
				{
					++PreparedView.SuccessfulDraws;
				}
				else
				{
					++PreparedView.RejectedDraws;
				}
			}
		};
		DrawBucket(PreparedView.Opaque, EStaticMeshBasePass::Opaque);
		DrawBucket(PreparedView.Masked, EStaticMeshBasePass::Masked);
		DrawBucket(PreparedView.Translucent, EStaticMeshBasePass::Translucent);
		PreparedView.Phase = EPreparedStaticMeshPhase::Executed;
		const bool bDrawCountersConserved = PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws;
		const bool bAllPreparedDrawsAttempted =
			PreparedView.AttemptedDraws == PreparedView.GetNumSections();
		check(bDrawCountersConserved);
		check(bAllPreparedDrawsAttempted);
	}

	auto FStaticMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		EStaticMeshBasePass Pass, const FPreparedStaticMeshDraw& Item,
		FPreparedStaticMeshView& PreparedView) -> void
	{
		++PreparedView.AttemptedDraws;
		const FPreparedStaticMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Item);
		const bool bComplete = Primitive != nullptr
			&& Primitive->PrimitiveId != InvalidPrimitiveSceneId
			&& Primitive->LOD != nullptr && Primitive->VertexFactory != nullptr
			&& Item.Section != nullptr && Item.Pass == Pass
			&& Item.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
			&& Item.ShaderMapIdentity == Item.Material.PipelineIdentity.ShaderMap
			&& Item.PipelineKey.Material == Item.Material.PipelineIdentity;
		if (!bComplete || !Item.bResourcesReady)
		{
			++PreparedView.RejectedDraws;
			return;
		}
		if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode,
			*Primitive, Item)) ++PreparedView.SuccessfulDraws;
		else ++PreparedView.RejectedDraws;
	}

	auto FStaticMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		EStaticMeshBasePass Pass, FPreparedStaticMeshView& PreparedView) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedStaticMeshPhase::ResourcesPrepared);
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = Pass == EStaticMeshBasePass::Opaque
			? PreparedView.Opaque : Pass == EStaticMeshBasePass::Masked
				? PreparedView.Masked : PreparedView.Translucent;
		for (const FPreparedStaticMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting, RenderMode,
				Pass, Draw, PreparedView);
	}

	auto FStaticMeshRenderer::FinalizeExecution_RenderThread(
		FPreparedStaticMeshView& PreparedView) -> void
	{
		check(PreparedView.Phase == EPreparedStaticMeshPhase::ResourcesPrepared);
		PreparedView.Phase = EPreparedStaticMeshPhase::Executed;
		check(PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws);
		check(PreparedView.AttemptedDraws == PreparedView.GetNumSections());
	}

	auto FStaticMeshRenderer::DrawSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRHIUniformBufferRange& Lighting,
		ERenderMode RenderMode,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		bool bShadowDepth
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		check(Primitive.LOD != nullptr && Primitive.VertexFactory != nullptr
			&& Item.Section != nullptr);
		const FStaticMeshLODResources& LOD = *Primitive.LOD;
		const FStaticMeshSection& Section = *Item.Section;
		const FMatrix& LocalToWorld = Primitive.LocalToWorld;
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderV3Binding& MaterialBinding = Item.MaterialBinding;
		const FLocalVertexFactory& VertexFactory = *Primitive.VertexFactory;
		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = ToShaderMatrix(
			View.ViewProjectionMatrix * LocalToWorld
		);
		TransformUniform.LocalToWorld =
			ToShaderMatrix(LocalToWorld);
		TransformUniform.NormalToWorld = ToShaderMatrix(
			Math::Transpose(Math::Inverse(LocalToWorld))
		);
		const float TransformDeterminant = glm::determinant(
			glm::mat3(FMatrix4f(LocalToWorld))
		);
		TransformUniform.TransformParams.x =
			TransformDeterminant < 0.0f ? -1.0f : 1.0f;
		const FRHIUniformBufferRange TransformUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform,
				sizeof(TransformUniform)
			);
		const FSplineMeshUniform SplineUniform = MakeSplineMeshUniform(
			Primitive.VertexDomain == EVertexDeformationDomain::Spline
				? Primitive.SplineDynamicData.Params : FSplineMeshParams{});
		const FRHIUniformBufferRange SplineUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&SplineUniform, sizeof(SplineUniform));

		VertexFactory.BindStreams(CommandList);
		CommandList.BindIndexBuffer(LOD.IndexBuffer.GetRHI(), 0);
		FState::FBaseResources* BaseResources =
			State->BaseResources.GetPayload();
		if (BaseResources == nullptr)
		{
			return false;
		}

		const FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey)
				: Item.PipelineKey;
		auto* PipelineEntry = bShadowDepth
			? State->ShadowPipelines.Find(EffectivePipelineKey)
			: State->Pipelines.Find(EffectivePipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr
			? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Pipeline == nullptr)
		{
			return false;
		}

		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
		if (bShadowDepth)
		{
			const FRHIRasterizerState Rasterizer =
				MakeShadowRasterizerState(Item.PipelineKey.Rasterizer);
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor,
				Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}

		if (Primitive.VertexDomain == EVertexDeformationDomain::Spline)
		{
			FSplineMeshVertexShader::FParameters Parameters;
			Parameters.Transform = TransformUniformBuffer;
			Parameters.SplineMesh = SplineUniformBuffer;
			SetShaderParameters(CommandList, Pipeline->SplineVertexShader, Parameters);
		}
		else
		{
			FStaticMeshVertexShader::FParameters Parameters;
			Parameters.Transform = TransformUniformBuffer;
			SetShaderParameters(CommandList, Pipeline->VertexShader, Parameters);
		}
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				!= EMaterialBlendMode::Masked)
		{
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FStaticMeshMaterialUniform MaterialUniform;
		MaterialUniform.BaseColor = MaterialBinding.BaseColor;
		MaterialUniform.EmissiveMetallic = FVector4f(
			MaterialBinding.Emissive,
			MaterialBinding.Metallic
		);
		MaterialUniform.NormalRoughness = FVector4f(
			MaterialBinding.Normal,
			MaterialBinding.Roughness
		);
		MaterialUniform.SurfaceParams = FVector4f(
			MaterialBinding.AmbientOcclusion,
			MaterialBinding.OpacityMask,
			RenderMode == ERenderMode::Lit
					&& Material.PipelineIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit ?
				1.0f :
				0.0f,
			0.0f
		);
		for (size_t Role = 0; Role < MaterialBinding.Textures.size(); ++Role)
		{
			MaterialUniform.UVTransforms[Role] = FVector4f(
				MaterialBinding.UVScales[Role].x,
				MaterialBinding.UVScales[Role].y,
				MaterialBinding.UVOffsets[Role].x,
				MaterialBinding.UVOffsets[Role].y
			);
		}
		MaterialUniform.UVChannels0 = FVector4f(
			MaterialBinding.UVChannels[0], MaterialBinding.UVChannels[1],
			MaterialBinding.UVChannels[2], MaterialBinding.UVChannels[3]
		);
		MaterialUniform.UVChannels1 = FVector4f(
			MaterialBinding.UVChannels[4], MaterialBinding.UVChannels[5],
			MaterialBinding.UVChannels[6], MaterialBinding.UVChannels[7]
		);
		MaterialUniform.UVRotations0 = FVector4f(
			MaterialBinding.UVRotations[0], MaterialBinding.UVRotations[1],
			MaterialBinding.UVRotations[2], MaterialBinding.UVRotations[3]
		);
		MaterialUniform.UVRotations1 = FVector4f(
			MaterialBinding.UVRotations[4], MaterialBinding.UVRotations[5],
			MaterialBinding.UVRotations[6], MaterialBinding.UVRotations[7]
		);
		const FRHIUniformBufferRange MaterialUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform,
				sizeof(MaterialUniform)
			);
		FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
		FragmentShaderParameters.Lighting = Lighting;
		FragmentShaderParameters.Material = MaterialUniformBuffer;
		auto ResolveTexture = [&](size_t Role, EDefaultTexture Fallback) {
			FRHITexture* Texture = MaterialBinding.Textures[Role] != nullptr ? MaterialBinding.Textures[Role]->GetReferencedTexture_RenderThread() : nullptr;
			return Texture != nullptr ? Texture : DefaultTextures.Get_RenderThread(Fallback);
		};
		FragmentShaderParameters.BaseColorTexture = ResolveTexture(0, EDefaultTexture::White);
		FragmentShaderParameters.NormalTexture = ResolveTexture(1, EDefaultTexture::FlatNormal);
		FragmentShaderParameters.MetallicTexture = ResolveTexture(2, EDefaultTexture::White);
		FragmentShaderParameters.RoughnessTexture = ResolveTexture(3, EDefaultTexture::White);
		FragmentShaderParameters.AmbientOcclusionTexture = ResolveTexture(4, EDefaultTexture::White);
		FragmentShaderParameters.EmissiveTexture = ResolveTexture(5, EDefaultTexture::Black);
		FragmentShaderParameters.OpacityTexture = ResolveTexture(6, EDefaultTexture::White);
		FragmentShaderParameters.OpacityMaskTexture = ResolveTexture(7, EDefaultTexture::White);
		std::array<FRHISampler*, 8> MaterialSamplers{};
		for (size_t Role = 0; Role < MaterialSamplers.size(); ++Role)
		{
			const size_t Key = GetMaterialSamplerKey(
				MaterialBinding.Samplers[Role]);
			const auto Entry = BaseResources->MaterialSamplerCache.find(Key);
			if (Entry == BaseResources->MaterialSamplerCache.end())
			{
				return false;
			}
			FSamplerRHIRef* Sampler = Entry->second.GetPayload();
			if (Sampler == nullptr)
			{
				return false;
			}
			MaterialSamplers[Role] = Sampler->GetReference();
		}
		FragmentShaderParameters.BaseColorSampler = MaterialSamplers[0];
		FragmentShaderParameters.NormalSampler = MaterialSamplers[1];
		FragmentShaderParameters.MetallicSampler = MaterialSamplers[2];
		FragmentShaderParameters.RoughnessSampler = MaterialSamplers[3];
		FragmentShaderParameters.AmbientOcclusionSampler = MaterialSamplers[4];
		FragmentShaderParameters.EmissiveSampler = MaterialSamplers[5];
		FragmentShaderParameters.OpacitySampler = MaterialSamplers[6];
		FragmentShaderParameters.OpacityMaskSampler = MaterialSamplers[7];
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked)
		{
			FStaticMeshShadowFragmentShader::FParameters ShadowParameters;
			ShadowParameters.Material = MaterialUniformBuffer;
			ShadowParameters.OpacityMaskTexture =
				FragmentShaderParameters.OpacityMaskTexture;
			ShadowParameters.OpacityMaskSampler = MaterialSamplers[7];
			SetShaderParameters(CommandList, Pipeline->ShadowFragmentShader,
				ShadowParameters);
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FRHITexture* EnvironmentIrradiance =
			EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* EnvironmentPrefiltered =
			EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* EnvironmentBrdfLut =
			EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler =
			EnvironmentLighting.GetSampler_RenderThread();
		const bool bHasCompleteEnvironment = EnvironmentIrradiance != nullptr
											 && EnvironmentPrefiltered != nullptr && EnvironmentBrdfLut != nullptr
											 && EnvironmentSampler != nullptr;
		FragmentShaderParameters.EnvironmentIrradiance = bHasCompleteEnvironment ? EnvironmentIrradiance : DefaultTextures.GetCube_RenderThread();
		FragmentShaderParameters.EnvironmentPrefiltered = bHasCompleteEnvironment ? EnvironmentPrefiltered : DefaultTextures.GetCube_RenderThread();
		FragmentShaderParameters.EnvironmentBrdfLut = bHasCompleteEnvironment ? EnvironmentBrdfLut : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
		FragmentShaderParameters.EnvironmentSampler = bHasCompleteEnvironment ? EnvironmentSampler : MaterialSamplers[0];
		FragmentShaderParameters.DirectionalShadowTexture =
			Item.DirectionalShadowTexture != nullptr
				? Item.DirectionalShadowTexture
				: DefaultTextures.GetArray_RenderThread();
		FragmentShaderParameters.DirectionalShadowSampler =
			Item.DirectionalShadowSampler != nullptr
				? Item.DirectionalShadowSampler : MaterialSamplers[0];
		SetShaderParameters(
			CommandList,
			Pipeline->FragmentShader,
			FragmentShaderParameters
		);
		CommandList.DrawIndexed(
			Section.IndexCount,
			Section.FirstIndex,
			0
		);
		return true;
	}

	auto FStaticMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->BaseResources.Reset();
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}

	FSkeletalMeshRenderer::FSkeletalMeshRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures,
		FEnvironmentLightingResources& InEnvironmentLighting)
		: Coordinator(InCoordinator), DefaultTextures(InDefaultTextures),
		  EnvironmentLighting(InEnvironmentLighting),
		  State(std::make_unique<FState>())
	{
	}

	FSkeletalMeshRenderer::~FSkeletalMeshRenderer() = default;

	auto FSkeletalMeshRenderer::EnsureBaseResources_RenderThread() -> bool
	{
		using FResult = TRenderResourceCreateResult<FState::FBaseResources>;
		return State->BaseResources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[]() -> FResult { return FResult::Success(FState::FBaseResources{}); },
			ReportRendererResourceCreateDiagnostic) != nullptr;
	}

	auto FSkeletalMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item,
		bool bShadowDepth) -> bool
	{
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		if (Base == nullptr || Primitive.VertexFactory == nullptr) return false;
		const FMaterialRenderData& Material = Item.Material;
		using FShaderResult = TRenderResourceCreateResult<FState::FShaderMapPayload>;
		auto& ShaderMapCache = bShadowDepth
			? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderEntry = ShaderMapCache.FindOrAdd(
			Material.PipelineIdentity.ShaderMap);
		FState::FShaderMapPayload* ShaderPayload = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &Material, bShadowDepth]() -> FShaderResult {
				const FMaterialShaderMapIdentity& Identity =
					Material.PipelineIdentity.ShaderMap;
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				Options.Macros.emplace_back("DURIN_SKELETAL_MESH", "1");
				Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE",
					std::to_string(static_cast<uint8>(Identity.BlendMode)));
				Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL",
					std::to_string(static_cast<uint8>(Identity.ShadingModel)));
				Options.Macros.emplace_back(
					"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
					std::to_string(std::bit_cast<uint32>(
						Identity.OpacityMaskThreshold)));
				FShaderType& VertexType = FSkeletalMeshVertexShader::StaticType();
				FShaderType& FragmentType = FStaticMeshFragmentShader::StaticType();
				FShaderType& ShadowFragmentType =
					FStaticMeshShadowFragmentShader::StaticType();
				FShaderType& OpaqueShadowFragmentType =
					FStaticMeshOpaqueShadowFragmentShader::StaticType();
				std::vector<const FShaderType*> Types{&VertexType};
				if (!bShadowDepth)
					Types.push_back(&FragmentType);
				else if (Identity.BlendMode == EMaterialBlendMode::Masked)
					Types.push_back(&ShadowFragmentType);
				else Types.push_back(&OpaqueShadowFragmentType);
				auto ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				auto* Vertex = static_cast<FSkeletalMeshVertexShader*>(
					ShaderMap->GetShader(&VertexType));
				auto* Fragment = !bShadowDepth
					? static_cast<FStaticMeshFragmentShader*>(
						ShaderMap->GetShader(&FragmentType)) : nullptr;
				auto* ShadowFragment = bShadowDepth
					&& Identity.BlendMode == EMaterialBlendMode::Masked
					? static_cast<FStaticMeshShadowFragmentShader*>(
						ShaderMap->GetShader(&ShadowFragmentType)) : nullptr;
				auto* OpaqueShadowFragment =
					bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
						? static_cast<FStaticMeshOpaqueShadowFragmentShader*>(
							ShaderMap->GetShader(&OpaqueShadowFragmentType)) : nullptr;
				if (Vertex == nullptr || (!bShadowDepth && Fragment == nullptr))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled map did not contain both typed shaders.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				if (bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked
					&& ShadowFragment == nullptr)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled masked shader map did not contain the shadow fragment shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
					&& OpaqueShadowFragment == nullptr)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled shader map did not contain the opaque shadow fragment shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				FState::FShaderMapPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				if (Fragment != nullptr)
					Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
				if (ShadowFragment != nullptr)
					Candidate.ShadowFragmentShader = {
						ShadowFragment, Candidate.ShaderMap.get()};
				if (OpaqueShadowFragment != nullptr)
					Candidate.OpaqueShadowFragmentShader = {
						OpaqueShadowFragment, Candidate.ShaderMap.get()};
				if (Candidate.VertexShader.GetRHIShader(false) == nullptr
					|| (!bShadowDepth
						&& Candidate.FragmentShader.GetRHIShader(false) == nullptr)
					|| (bShadowDepth
						&& Identity.BlendMode == EMaterialBlendMode::Masked
						&& Candidate.ShadowFragmentShader.GetRHIShader(false) == nullptr)
					|| (bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked
						&& Candidate.OpaqueShadowFragmentShader.GetRHIShader(false) == nullptr))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"RHI shader creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device));
				return FShaderResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (ShaderPayload == nullptr) return false;

		using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
		const FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey)
				: Item.PipelineKey;
		auto& PipelineCache = bShadowDepth
			? State->ShadowPipelines : State->Pipelines;
		auto& PipelineEntry = PipelineCache.FindOrAdd(EffectivePipelineKey);
		FRenderResourceGeneration Generation =
			Coordinator.GetGeneration_RenderThread();
		Generation.Shader = ShaderEntry.Slot.GetPayloadGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			Generation,
			[&EffectivePipelineKey, &PipelineEntry, ShaderPayload,
			 bShadowDepth,
				VertexFactory = Primitive.VertexFactory]() -> FPipelineResult {
				FState::FPipelinePayload Candidate;
				Candidate.ShaderMap = ShaderPayload->ShaderMap;
				Candidate.VertexShader = ShaderPayload->VertexShader;
				Candidate.FragmentShader = ShaderPayload->FragmentShader;
				Candidate.ShadowFragmentShader =
					ShaderPayload->ShadowFragmentShader;
				Candidate.OpaqueShadowFragmentShader =
					ShaderPayload->OpaqueShadowFragmentShader;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = bShadowDepth
					? RenderTargetLayouts::MakeDirectionalShadowDepth()
					: RenderTargetLayouts::MakeSceneTargets();
				Initializer.BoundShaders.VertexShader =
					Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth
					? (EffectivePipelineKey.Material.ShaderMap.BlendMode
							== EMaterialBlendMode::Masked
						? Candidate.ShadowFragmentShader.GetRHIShader()
						: Candidate.OpaqueShadowFragmentShader.GetRHIShader())
					: Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory->GetDeclaration();
				Initializer.RasterizerState = EffectivePipelineKey.Rasterizer;
				Initializer.DepthStencilState = EffectivePipelineKey.Depth;
				if (!bShadowDepth)
					Initializer.ColorBlendStates[0] = EffectivePipelineKey.ColorBlend;
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					FName(std::format("SkeletalMeshPipeline_{}", PipelineEntry.Index)),
					Initializer);
				if (Candidate.PipelineState == nullptr)
					return FPipelineResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"SkeletalMeshPipeline", GetIdentityText(EffectivePipelineKey),
						"Graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device));
				return FPipelineResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (Pipeline == nullptr) return false;

		for (const FMaterialSamplerState& SamplerState : Item.MaterialBinding.Samplers)
		{
			auto Entry = Base->MaterialSamplerCache.try_emplace(
				GetMaterialSamplerKey(SamplerState),
				ERenderResourceGenerationDependency::Device).first;
			using FSamplerResult = TRenderResourceCreateResult<FSamplerRHIRef>;
			if (Entry->second.Resolve(Coordinator.GetGeneration_RenderThread(),
				[SamplerState]() -> FSamplerResult {
					FSamplerRHIRef Candidate =
						RHICreateSampler(MakeMaterialSamplerDesc(SamplerState));
					return Candidate != nullptr
						? FSamplerResult::Success(std::move(Candidate))
						: FSamplerResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SkeletalMeshMaterialSampler", "material sampler",
							"RHI sampler creation returned null.",
							ERenderResourceGenerationDependency::Device));
				}, ReportRendererResourceCreateDiagnostic) == nullptr) return false;
		}
		return true;
	}

	auto FSkeletalMeshRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSkeletalMeshView& PreparedView) -> bool
	{
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::Prepared);
		EnsureBaseResources_RenderThread();
		constexpr uint64 PaletteBudget = 64ull * 1024ull * 1024ull;
		uint64 RequestedBytes = 0;
		struct FPaletteKey
		{
			uint64 PrimitiveId = 0;
			uint64 Revision = 0;
			auto operator==(const FPaletteKey&) const -> bool = default;
		};
		struct FPaletteKeyHash
		{
			auto operator()(const FPaletteKey& Key) const -> size_t
			{
				return static_cast<size_t>(Key.PrimitiveId
					^ (Key.Revision + 0x9e3779b97f4a7c15ull
						+ (Key.PrimitiveId << 6) + (Key.PrimitiveId >> 2)));
			}
		};
		std::unordered_map<FPaletteKey, FRHIStorageBufferRange, FPaletteKeyHash>
			Uploaded;
		for (FPreparedSkeletalMeshPrimitive& Primitive : PreparedView.Primitives)
		{
			if (Primitive.Pose == nullptr) { ++PreparedView.RejectedPalettes; continue; }
			const uint64 Bytes = Primitive.Pose->Matrices.size() * sizeof(FMatrix4f);
			const FPaletteKey Key{
				Primitive.PrimitiveId.Value, Primitive.Pose->Revision};
			if (const auto It = Uploaded.find(Key); It != Uploaded.end())
			{
				Primitive.PaletteRange = It->second;
				++PreparedView.ReusedPalettes;
				continue;
			}
			if (Bytes == 0 || RequestedBytes + Bytes > PaletteBudget)
			{
				++PreparedView.RejectedPalettes;
				continue;
			}
			Primitive.PaletteRange = CommandList.AllocateDynamicStorageBuffer(
				Primitive.Pose->Matrices.data(), static_cast<uint32>(Bytes));
			if (Primitive.PaletteRange.Buffer == nullptr
				|| Primitive.PaletteRange.Size != Bytes)
			{
				Primitive.PaletteRange = {};
				++PreparedView.RejectedPalettes;
				continue;
			}
			const std::array Transition{FRHIBufferTransition{
				Primitive.PaletteRange.Buffer, Primitive.PaletteRange.Offset,
				Primitive.PaletteRange.Size, ERHIAccess::HostWrite,
				ERHIAccess::GraphicsShaderRead}};
			CommandList.TransitionBuffers(Transition);
			Uploaded.emplace(Key, Primitive.PaletteRange);
			RequestedBytes += Bytes;
			++PreparedView.UploadedPalettes;
			PreparedView.UploadedPaletteMatrices += Primitive.Pose->Matrices.size();
			PreparedView.UploadedPaletteBytes += Bytes;
		}
		auto PrepareBucket = [&](auto& Bucket) {
			for (FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.ResourcePreparationAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				Draw.bResourcesReady = Primitive != nullptr
					&& Primitive->PaletteRange.Buffer != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Draw);
				if (Draw.bResourcesReady)
					++PreparedView.ResourcePreparationSuccessfulDraws;
			}
		};
		PrepareBucket(PreparedView.Opaque);
		PrepareBucket(PreparedView.Masked);
		PrepareBucket(PreparedView.Translucent);
		PreparedView.ResourcePreparationRejectedDraws =
			PreparedView.ResourcePreparationAttemptedDraws
				- PreparedView.ResourcePreparationSuccessfulDraws;
		PreparedView.Phase = EPreparedSkeletalMeshPhase::ResourcesPrepared;
		check(PreparedView.RequestedPaletteUploads
			== PreparedView.UploadedPalettes + PreparedView.ReusedPalettes
				+ PreparedView.RejectedPalettes);
		check(PreparedView.UploadedPaletteBytes
			== PreparedView.UploadedPaletteMatrices * sizeof(FMatrix4f));
		check(PreparedView.ResourcePreparationAttemptedDraws
			== PreparedView.ResourcePreparationSuccessfulDraws
				+ PreparedView.ResourcePreparationRejectedDraws);
		return PreparedView.ResourcePreparationRejectedDraws == 0;
	}

	auto FSkeletalMeshRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedSkeletalMeshView& BaseView,
		FPreparedSkeletalMeshView& PreparedView) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		check(BaseView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::Prepared);
		check(PreparedView.Translucent.empty());
		EnsureBaseResources_RenderThread();
		for (FPreparedSkeletalMeshPrimitive& Primitive : PreparedView.Primitives)
		{
			const auto Match = std::ranges::find_if(
				BaseView.Primitives,
				[&Primitive](const FPreparedSkeletalMeshPrimitive& Candidate) {
					return Candidate.PrimitiveId == Primitive.PrimitiveId
						&& Candidate.Pose != nullptr && Primitive.Pose != nullptr
						&& Candidate.Pose->Revision == Primitive.Pose->Revision;
				});
			if (Match != BaseView.Primitives.end()
				&& Match->PaletteRange.Buffer != nullptr)
			{
				Primitive.PaletteRange = Match->PaletteRange;
				++PreparedView.ReusedPalettes;
			}
			else
			{
				++PreparedView.RejectedPalettes;
			}
		}
		for (auto* Bucket : {&PreparedView.Opaque, &PreparedView.Masked})
		{
			for (FPreparedSkeletalMeshDraw& Draw : *Bucket)
			{
				++PreparedView.ResourcePreparationAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				Draw.bResourcesReady = Primitive != nullptr
					&& Primitive->PaletteRange.Buffer != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Draw, true);
				PreparedView.ResourcePreparationSuccessfulDraws +=
					Draw.bResourcesReady ? 1u : 0u;
			}
		}
		PreparedView.ResourcePreparationRejectedDraws =
			PreparedView.ResourcePreparationAttemptedDraws
				- PreparedView.ResourcePreparationSuccessfulDraws;
		PreparedView.Phase = EPreparedSkeletalMeshPhase::ResourcesPrepared;
		return PreparedView.ResourcePreparationRejectedDraws == 0;
	}

	auto FSkeletalMeshRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		FPreparedSkeletalMeshView& PreparedView) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		for (const auto* Bucket : {&PreparedView.Opaque, &PreparedView.Masked})
		{
			for (const FPreparedSkeletalMeshDraw& Draw : *Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && Draw.bResourcesReady
					&& DrawSection_RenderThread(
						CommandList, ShadowView, FallbackLighting,
						ERenderMode::Unlit, *Primitive, Draw, true))
					++PreparedView.SuccessfulDraws;
				else
					++PreparedView.RejectedDraws;
			}
		}
		PreparedView.Phase = EPreparedSkeletalMeshPhase::Executed;
		check(PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws);
	}

	auto FSkeletalMeshRenderer::Execute_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		FPreparedSkeletalMeshView& PreparedView) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
		{
			PreparedView.Phase = EPreparedSkeletalMeshPhase::Executed;
			return;
		}
		auto DrawBucket = [&](const auto& Bucket, EStaticMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bComplete = Primitive != nullptr
					&& Primitive->PrimitiveId != InvalidPrimitiveSceneId
					&& Primitive->RenderData != nullptr
					&& Primitive->VertexFactory != nullptr
					&& Primitive->Pose != nullptr
					&& Primitive->PaletteRange.Buffer != nullptr
					&& Draw.Section != nullptr && Draw.Pass == Pass
					&& Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
					&& Draw.ShaderMapIdentity
						== Draw.Material.PipelineIdentity.ShaderMap;
				if (!bComplete || !Draw.bResourcesReady)
				{
					++PreparedView.RejectedDraws;
					continue;
				}
				if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode,
					*Primitive, Draw)) ++PreparedView.SuccessfulDraws;
				else ++PreparedView.RejectedDraws;
			}
		};
		DrawBucket(PreparedView.Opaque, EStaticMeshBasePass::Opaque);
		DrawBucket(PreparedView.Masked, EStaticMeshBasePass::Masked);
		DrawBucket(PreparedView.Translucent, EStaticMeshBasePass::Translucent);
		PreparedView.Phase = EPreparedSkeletalMeshPhase::Executed;
		check(PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws);
		check(PreparedView.AttemptedDraws == PreparedView.GetNumSections());
	}

	auto FSkeletalMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		EStaticMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw,
		FPreparedSkeletalMeshView& PreparedView) -> void
	{
		++PreparedView.AttemptedDraws;
		const FPreparedSkeletalMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Draw);
		const bool bComplete = Primitive != nullptr
			&& Primitive->PrimitiveId != InvalidPrimitiveSceneId
			&& Primitive->RenderData != nullptr
			&& Primitive->VertexFactory != nullptr && Primitive->Pose != nullptr
			&& Primitive->PaletteRange.Buffer != nullptr
			&& Draw.Section != nullptr && Draw.Pass == Pass
			&& Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
			&& Draw.ShaderMapIdentity == Draw.Material.PipelineIdentity.ShaderMap;
		if (!bComplete || !Draw.bResourcesReady)
		{
			++PreparedView.RejectedDraws;
			return;
		}
		if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode,
			*Primitive, Draw)) ++PreparedView.SuccessfulDraws;
		else ++PreparedView.RejectedDraws;
	}

	auto FSkeletalMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		EStaticMeshBasePass Pass, FPreparedSkeletalMeshView& PreparedView) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = Pass == EStaticMeshBasePass::Opaque
			? PreparedView.Opaque : Pass == EStaticMeshBasePass::Masked
				? PreparedView.Masked : PreparedView.Translucent;
		for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting, RenderMode,
				Pass, Draw, PreparedView);
	}

	auto FSkeletalMeshRenderer::FinalizeExecution_RenderThread(
		FPreparedSkeletalMeshView& PreparedView) -> void
	{
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		PreparedView.Phase = EPreparedSkeletalMeshPhase::Executed;
		check(PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws);
		check(PreparedView.AttemptedDraws == PreparedView.GetNumSections());
	}

	auto FSkeletalMeshRenderer::DrawSection_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode,
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item,
		bool bShadowDepth) -> bool
	{
		const FSkeletalMeshRenderData& Data = *Primitive.RenderData;
		const FSkeletalMeshRenderSection& Section = *Item.Section;
		const FMatrix& LocalToWorld = Primitive.LocalToWorld;
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderV3Binding& Binding = Item.MaterialBinding;
		FStaticMeshTransformUniform Transform;
		Transform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * LocalToWorld);
		Transform.LocalToWorld = ToShaderMatrix(LocalToWorld);
		Transform.NormalToWorld = ToShaderMatrix(
			Math::Transpose(Math::Inverse(LocalToWorld)));
		Transform.TransformParams.x = glm::determinant(
			glm::mat3(FMatrix4f(LocalToWorld))) < 0.0f ? -1.0f : 1.0f;
		Transform.TransformParams.y = static_cast<float>(
			Primitive.Pose->Matrices.size());
		const FRHIUniformBufferRange TransformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Transform, sizeof(Transform));

		Primitive.VertexFactory->BindStreams(CommandList);
		CommandList.BindIndexBuffer(Data.IndexBuffer.GetRHI(), 0);
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		const FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey)
				: Item.PipelineKey;
		auto* PipelineEntry = bShadowDepth
			? State->ShadowPipelines.Find(EffectivePipelineKey)
			: State->Pipelines.Find(EffectivePipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr
			? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Base == nullptr || Pipeline == nullptr) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
		if (bShadowDepth)
		{
			const FRHIRasterizerState Rasterizer =
				MakeShadowRasterizerState(Item.PipelineKey.Rasterizer);
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor,
				Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}
		FSkeletalMeshVertexShader::FParameters VertexParameters;
		VertexParameters.Transform = TransformBuffer;
		VertexParameters.SkinPalette = Primitive.PaletteRange;
		SetShaderParameters(CommandList, Pipeline->VertexShader, VertexParameters);
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				!= EMaterialBlendMode::Masked)
		{
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FStaticMeshMaterialUniform MaterialUniform;
		MaterialUniform.BaseColor = Binding.BaseColor;
		MaterialUniform.EmissiveMetallic = FVector4f(
			Binding.Emissive, Binding.Metallic);
		MaterialUniform.NormalRoughness = FVector4f(
			Binding.Normal, Binding.Roughness);
		MaterialUniform.SurfaceParams = FVector4f(
			Binding.AmbientOcclusion, Binding.OpacityMask,
			RenderMode == ERenderMode::Lit
				&& Material.PipelineIdentity.ShaderMap.ShadingModel
					== EMaterialShadingModel::Lit ? 1.0f : 0.0f, 0.0f);
		for (size_t Role = 0; Role < Binding.Textures.size(); ++Role)
			MaterialUniform.UVTransforms[Role] = FVector4f(
				Binding.UVScales[Role].x, Binding.UVScales[Role].y,
				Binding.UVOffsets[Role].x, Binding.UVOffsets[Role].y);
		MaterialUniform.UVChannels0 = FVector4f(Binding.UVChannels[0],
			Binding.UVChannels[1], Binding.UVChannels[2], Binding.UVChannels[3]);
		MaterialUniform.UVChannels1 = FVector4f(Binding.UVChannels[4],
			Binding.UVChannels[5], Binding.UVChannels[6], Binding.UVChannels[7]);
		MaterialUniform.UVRotations0 = FVector4f(Binding.UVRotations[0],
			Binding.UVRotations[1], Binding.UVRotations[2], Binding.UVRotations[3]);
		MaterialUniform.UVRotations1 = FVector4f(Binding.UVRotations[4],
			Binding.UVRotations[5], Binding.UVRotations[6], Binding.UVRotations[7]);
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform, sizeof(MaterialUniform));
		FStaticMeshFragmentShader::FParameters FragmentParameters;
		FragmentParameters.Lighting = Lighting;
		FragmentParameters.Material = MaterialBuffer;
		auto ResolveTexture = [&](size_t Role, EDefaultTexture Fallback) {
			FRHITexture* Texture = Binding.Textures[Role] != nullptr
				? Binding.Textures[Role]->GetReferencedTexture_RenderThread() : nullptr;
			return Texture != nullptr ? Texture
				: DefaultTextures.Get_RenderThread(Fallback);
		};
		FragmentParameters.BaseColorTexture = ResolveTexture(0, EDefaultTexture::White);
		FragmentParameters.NormalTexture = ResolveTexture(1, EDefaultTexture::FlatNormal);
		FragmentParameters.MetallicTexture = ResolveTexture(2, EDefaultTexture::White);
		FragmentParameters.RoughnessTexture = ResolveTexture(3, EDefaultTexture::White);
		FragmentParameters.AmbientOcclusionTexture = ResolveTexture(4, EDefaultTexture::White);
		FragmentParameters.EmissiveTexture = ResolveTexture(5, EDefaultTexture::Black);
		FragmentParameters.OpacityTexture = ResolveTexture(6, EDefaultTexture::White);
		FragmentParameters.OpacityMaskTexture = ResolveTexture(7, EDefaultTexture::White);
		std::array<FRHISampler*, 8> Samplers{};
		for (size_t Role = 0; Role < Samplers.size(); ++Role)
		{
			const auto It = Base->MaterialSamplerCache.find(
				GetMaterialSamplerKey(Binding.Samplers[Role]));
			if (It == Base->MaterialSamplerCache.end()) return false;
			FSamplerRHIRef* Sampler = It->second.GetPayload();
			if (Sampler == nullptr) return false;
			Samplers[Role] = Sampler->GetReference();
		}
		FragmentParameters.BaseColorSampler = Samplers[0];
		FragmentParameters.NormalSampler = Samplers[1];
		FragmentParameters.MetallicSampler = Samplers[2];
		FragmentParameters.RoughnessSampler = Samplers[3];
		FragmentParameters.AmbientOcclusionSampler = Samplers[4];
		FragmentParameters.EmissiveSampler = Samplers[5];
		FragmentParameters.OpacitySampler = Samplers[6];
		FragmentParameters.OpacityMaskSampler = Samplers[7];
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked)
		{
			FStaticMeshShadowFragmentShader::FParameters ShadowParameters;
			ShadowParameters.Material = MaterialBuffer;
			ShadowParameters.OpacityMaskTexture =
				FragmentParameters.OpacityMaskTexture;
			ShadowParameters.OpacityMaskSampler = Samplers[7];
			SetShaderParameters(CommandList, Pipeline->ShadowFragmentShader,
				ShadowParameters);
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FRHITexture* Irradiance = EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* Prefiltered = EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* Brdf = EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler = EnvironmentLighting.GetSampler_RenderThread();
		const bool bEnvironment = Irradiance && Prefiltered && Brdf && EnvironmentSampler;
		FragmentParameters.EnvironmentIrradiance = bEnvironment
			? Irradiance : DefaultTextures.GetCube_RenderThread();
		FragmentParameters.EnvironmentPrefiltered = bEnvironment
			? Prefiltered : DefaultTextures.GetCube_RenderThread();
		FragmentParameters.EnvironmentBrdfLut = bEnvironment
			? Brdf : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
		FragmentParameters.EnvironmentSampler = bEnvironment
			? EnvironmentSampler : Samplers[0];
		FragmentParameters.DirectionalShadowTexture =
			Item.DirectionalShadowTexture != nullptr
				? Item.DirectionalShadowTexture
				: DefaultTextures.GetArray_RenderThread();
		FragmentParameters.DirectionalShadowSampler =
			Item.DirectionalShadowSampler != nullptr
				? Item.DirectionalShadowSampler : Samplers[0];
		SetShaderParameters(CommandList, Pipeline->FragmentShader,
			FragmentParameters);
		CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
		return true;
	}

	auto FSkeletalMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		State->BaseResources.Reset();
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin
