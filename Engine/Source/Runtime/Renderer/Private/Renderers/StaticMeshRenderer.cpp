#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/ViewPreparationMath.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Engine/FPrimitiveSceneProxy.h"
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
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshFragmentShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Fragment,
				"FragmentMain"
			);
		};

		struct FStaticMeshTransformUniform
		{
			FMatrix4f LocalToClip{1.0f};
			FMatrix4f LocalToWorld{1.0f};
			FMatrix4f NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FStaticMeshLightingUniform
		{
			FVector4f LightDirection{-0.5f, -0.5f, -1.0f, 0.0f};
			FVector4f LightColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
			FVector4f ViewPosition{0.0f};
		};

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
				"{},polygon={},cull={},front={},depth-test={},depth-write={},depth-compare={},blend={},color-src={},color-dst={},color-op={},alpha-src={},alpha-dst={},alpha-op={},write-mask={}",
				GetIdentityText(Identity.Material),
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

		auto MakeStaticMeshDrawSortKey(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Draw) -> FStaticMeshDrawSortKey
		{
			FStaticMeshDrawSortKey Result;
			const FMaterialPipelineIdentity& Material = Draw.PipelineKey.Material;
			const FMaterialShaderMapIdentity& Shader = Material.ShaderMap;
			const FGuid& LayoutId = Shader.RenderLayout.Id;
			Result.Pipeline = {
				static_cast<uint32>(Draw.Pass), Shader.RenderLayout.Version,
				LayoutId.A, LayoutId.B, LayoutId.C, LayoutId.D,
				static_cast<uint32>(Shader.BlendMode),
				static_cast<uint32>(Shader.ShadingModel),
				std::bit_cast<uint32>(Shader.OpacityMaskThreshold),
				Material.bTwoSided ? 1u : 0u,
				static_cast<uint32>(Material.DepthWritePolicy),
				static_cast<uint32>(Draw.PipelineKey.Rasterizer.PolygonMode),
				static_cast<uint32>(Draw.PipelineKey.Rasterizer.CullMode),
				static_cast<uint32>(Draw.PipelineKey.Rasterizer.FrontFace),
				Draw.PipelineKey.Depth.bEnableTest ? 1u : 0u,
				Draw.PipelineKey.Depth.bEnableWrite ? 1u : 0u,
				static_cast<uint32>(Draw.PipelineKey.Depth.CompareOp),
				Draw.PipelineKey.ColorBlend.bEnable ? 1u : 0u,
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.SrcColorFactor),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.DstColorFactor),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.ColorOp),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.SrcAlphaFactor),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.DstAlphaFactor),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.AlphaOp),
				static_cast<uint32>(Draw.PipelineKey.ColorBlend.ColorWriteMask)};
			const std::span<const std::byte> UniformPayload =
				Draw.Material.Representation.GetUniformPayload();
			Result.MaterialUniform.reserve(UniformPayload.size());
			for (const std::byte Byte : UniformPayload)
			{
				Result.MaterialUniform.push_back(std::to_integer<uint8>(Byte));
			}

			if (Primitive.VertexFactory != nullptr)
			{
				Result.VertexFactory[0] =
					Primitive.VertexFactory->GetData().NumVertices;
				const FVertexDeclarationElementList Elements =
					Primitive.VertexFactory->GetDeclarationElements();
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
			if (Draw.Section != nullptr)
			{
				Result.Geometry = {
					Draw.Section->FirstIndex, Draw.Section->IndexCount,
					Draw.Section->MinVertexIndex, Draw.Section->MaxVertexIndex,
					Draw.Section->MaterialSlotIndex,
					static_cast<uint32>(
						Primitive.LOD->IndexBuffer.GetIndices().size())};
			}
			Result.PrimitiveId = Primitive.PrimitiveId.Value;
			Result.SelectedLODIndex = Primitive.SelectedLODIndex;
			Result.SectionIndex = Draw.SectionIndex;
			return Result;
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
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
		};

		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		TRenderResourceCreationSlot<FBaseResources> BaseResources{
			ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<
			FMaterialShaderMapIdentity,
			FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FEffectiveStaticMeshPipelineKey,
			FPipelinePayload>
			Pipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
			};
	};

	auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode
	) -> FPreparedStaticMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(),
			"StaticMesh preparation must occur before the scene render pass.");
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(SceneInfos.size());
		for (const FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			++Result.VisibleCandidates;
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				continue;
			}
			check(SceneInfo->GetKind() == EPrimitiveSceneProxyKind::StaticMesh);
			const FStaticMeshSceneProxy& Proxy = SceneInfo->GetStaticMeshProxy();
			const FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
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

				const FMaterialRenderData& ResolvedMaterial =
					Proxy.ResolveMaterialRenderData_RenderThread(
						Section.MaterialSlotIndex
					);
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
			Result.SelectedSections += PreparedSectionCount;
			const size_t HistogramSize = RenderData->LODResources.size();
			Result.RequestedLODHistogram.resize(
				std::max(Result.RequestedLODHistogram.size(), HistogramSize));
			Result.SelectedLODHistogram.resize(
				std::max(Result.SelectedLODHistogram.size(), HistogramSize));
			++Result.RequestedLODHistogram[RequestedLODIndex];
			++Result.SelectedLODHistogram[SelectedLODIndex];
		}
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

	auto FStaticMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item) -> bool
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
		auto& ShaderMapEntry = State->ShaderMaps.FindOrAdd(
			Material.PipelineIdentity.ShaderMap);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material]() -> FShaderMapResult {
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
					FShaderType& VertexShaderType =
						FStaticMeshVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FStaticMeshFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
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
					auto* VertexShader = static_cast<FStaticMeshVertexShader*>(
						ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader = static_cast<FStaticMeshFragmentShader*>(
						ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"Compiled shader map did not contain both typed shaders.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual));
					}
					FState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader = TShaderRef<FStaticMeshVertexShader>(
						VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader = TShaderRef<FStaticMeshFragmentShader>(
						FragmentShader, Candidate.ShaderMap.get());
					if (Candidate.VertexShader.GetRHIShader(false) == nullptr
						|| Candidate.FragmentShader.GetRHIShader(false) == nullptr)
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
		auto& PipelineEntry = State->Pipelines.FindOrAdd(Item.PipelineKey);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderMapEntry.Slot.GetPayloadGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&Item, &PipelineEntry, ShaderMapPayload,
			 &VertexFactory]() -> FPipelineResult {
				const FEffectiveStaticMeshPipelineKey& Identity = Item.PipelineKey;
				FState::FPipelinePayload Candidate;
				Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
				Candidate.VertexShader = ShaderMapPayload->VertexShader;
				Candidate.FragmentShader = ShaderMapPayload->FragmentShader;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeSceneTargets();
				Initializer.BoundShaders.VertexShader =
					Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader =
					Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory.GetDeclaration();
				Initializer.RasterizerState = Identity.Rasterizer;
				Initializer.DepthState = Identity.Depth;
				Initializer.ColorBlendState = Identity.ColorBlend;
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
		const FDirectionalLightSceneData& Light,
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
						CommandList, View, Light, RenderMode, *Primitive, Item))
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

	auto FStaticMeshRenderer::DrawSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FDirectionalLightSceneData& Light,
		ERenderMode RenderMode,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item
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

		FStaticMeshLightingUniform LightingUniform;
		LightingUniform.LightDirection = FVector4f(
			FVector3f(Light.Direction),
			0.0f
		);
		LightingUniform.LightColorIntensity =
			FVector4f(Light.Color, Light.Intensity);
		LightingUniform.ViewPosition = FVector4f(
			FVector3f(View.ViewLocation),
			0.0f
		);
		const FRHIUniformBufferRange LightingUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&LightingUniform,
				sizeof(LightingUniform)
			);

		VertexFactory.BindStreams(CommandList);
		CommandList.BindIndexBuffer(LOD.IndexBuffer.GetRHI(), 0);
		FState::FBaseResources* BaseResources =
			State->BaseResources.GetPayload();
		if (BaseResources == nullptr)
		{
			return false;
		}

		auto* PipelineEntry = State->Pipelines.Find(Item.PipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr
			? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Pipeline == nullptr)
		{
			return false;
		}

		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);

		FStaticMeshVertexShader::FParameters VertexShaderParameters;
		VertexShaderParameters.Transform = TransformUniformBuffer;
		SetShaderParameters(
			CommandList,
			Pipeline->VertexShader,
			VertexShaderParameters
		);

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
		FragmentShaderParameters.Lighting = LightingUniformBuffer;
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
		State->Pipelines.Reset();
	}
} // namespace Durin
