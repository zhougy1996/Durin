#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"
#include "Renderers/MeshPipelineDescription.h"

namespace Durin
{
	enum class EGeometryResolutionStatus : uint8
	{
		Complete,
		Partial
	};

	struct FGeometryResolutionResult
	{
		EGeometryResolutionStatus Status = EGeometryResolutionStatus::Complete;
		size_t AttemptedDraws = 0;
		size_t ResolvedDraws = 0;
		size_t RejectedDraws = 0;

		operator bool() const
		{
			return Status == EGeometryResolutionStatus::Complete;
		}
	};

	struct FGeometryExecutionResult
	{
		bool bComplete = true;
		bool bRenderedGeometry = false;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t SkippedDraws = 0;
	};

	enum class EMeshBasePass : uint8
	{
		Opaque,
		Masked,
		Translucent,
	};

	enum class ERenderPreparationMode : uint8
	{
		Full,
		ShadowDepth,
	};

	enum class EVertexDeformationDomain : uint8
	{
		Local,
		Spline
	};

	struct FMeshShaderMapKey
	{
		FMeshMaterialShaderMapIdentity Material;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		FXxHash64 FactoryKey;
		FXxHash64 LayoutKey;
		auto operator==(const FMeshShaderMapKey&) const -> bool = default;
	};

	struct FEffectiveMeshPipelineKey
	{
		FMaterialPlanningPassIdentity Material;
		FRHIRasterizerState Rasterizer;
		FRHIDepthStencilState Depth;
		FRHIColorBlendState ColorBlend;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		FXxHash64 FactoryKey;
		FXxHash64 LayoutKey;
		bool bHybridRetained = false;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;

		auto operator==(const FEffectiveMeshPipelineKey&) const -> bool = default;
	};

	// Retains the publication rather than allocating another uniform payload per
	// sort key. Ordering is still by bytes, independent of publication order.
	struct FMaterialUniformSortKey
	{
		FMaterialRenderRepresentation Representation;
		auto operator<=>(const FMaterialUniformSortKey& Other) const -> std::strong_ordering
		{
			if (Representation.GetRecordId() == Other.Representation.GetRecordId())
				return std::strong_ordering::equal;
			const auto Left = Representation.GetUniformPayload();
			const auto Right = Other.Representation.GetUniformPayload();
			return std::lexicographical_compare_three_way(Left.begin(), Left.end(), Right.begin(), Right.end());
		}
		auto operator==(const FMaterialUniformSortKey& Other) const -> bool
		{
			return (*this <=> Other) == 0;
		}
	};

	// Complete ordering facts with owned immutable payloads. Stable identity is kept last so state
	// grouping happens before deterministic primitive/section tie breaking.
	struct FMeshDrawSortKey
	{
		std::array<uint32, 35> Pipeline{};
		FMaterialUniformSortKey MaterialUniform;
		// Owned values; trailing empty attributes need no storage.
		std::vector<uint32> VertexFactory;
		std::array<uint32, 6> Geometry{};
		uint64 PrimitiveId = 0;
		uint64 BatchId = 0;
		uint32 SelectedLODIndex = 0;
		uint64 SectionIndex = 0;

		auto operator<=>(const FMeshDrawSortKey&) const = default;
	};

	// Retains stable ordering arrays through an aliasing template reference.
	struct FVisibleMeshDrawSortKey
	{
		std::shared_ptr<const FMeshDrawSortKey> State;
		uint64 PrimitiveId = 0;
		uint64 BatchId = 0;
		uint32 SelectedLODIndex = 0;
		auto GetState() const -> const FMeshDrawSortKey&
		{
			static const FMeshDrawSortKey Empty;
			return State ? *State : Empty;
		}
		auto operator<=>(const FVisibleMeshDrawSortKey& Other) const
		{
			if (State == Other.State)
				return std::tie(PrimitiveId, BatchId, SelectedLODIndex)
					<=> std::tie(Other.PrimitiveId, Other.BatchId, Other.SelectedLODIndex);
			const auto& A = GetState();
			const auto& B = Other.GetState();
			return std::tie(A.Pipeline, A.MaterialUniform, A.VertexFactory, A.Geometry,
				PrimitiveId, BatchId, SelectedLODIndex, A.SectionIndex)
				<=> std::tie(B.Pipeline, B.MaterialUniform, B.VertexFactory, B.Geometry,
					Other.PrimitiveId, Other.BatchId, Other.SelectedLODIndex, B.SectionIndex);
		}
		auto operator==(const FVisibleMeshDrawSortKey& Other) const -> bool { return (*this <=> Other) == 0; }
	};


	class FRHIShaderParameterBatch;
	namespace RendererPrivate { struct FMeshDrawShaderBindings; struct FMeshDrawCommand; struct FCachedMeshDrawCommandInfo; struct FMeshPassPipeline; class FDynamicMeshDrawCommandStorage; }

	struct FShaderBindingState
	{
		FRHIShader* Shader = nullptr;
		std::vector<FRHIShaderParameterResource> Parameters;
		std::vector<FRHIShaderParameterResource> Changed;
	};
	struct FMeshDrawVertexStream
	{
		uint8 StreamIndex = 0;
		FRHIBuffer* VertexBuffer = nullptr;
		uint32 Offset = 0;
		uint16 Stride = 0;
		auto operator==(const FMeshDrawVertexStream&) const -> bool = default;
	};
	// One uninterrupted recording span; reset after foreign RHI state changes.
	struct FMeshDrawCommandStateCache
	{
		const void* Pipeline = nullptr;
		FShaderBindingState Vertex, Fragment;
		std::vector<FMeshDrawVertexStream> VertexStreams;
		FRHIBuffer* IndexBuffer = nullptr;
		uint32 IndexBufferOffset = 0;
		std::optional<std::array<float, 3>> DepthBias;
	};

	// Index into one frame's pass list. The storage identity rejects stale/cross-view handles.
	struct FMeshDrawCommandHandle
	{
		RendererPrivate::FMeshPassKey Pass;
		size_t Index = std::numeric_limits<size_t>::max();
		uint64 StorageId = 0;
		explicit operator bool() const { return StorageId != 0 && Index != std::numeric_limits<size_t>::max(); }
		auto operator==(const FMeshDrawCommandHandle&) const -> bool = default;
	};
	struct FResolvedMeshDrawRecord
	{
		std::optional<FMaterialRenderBinding> MaterialBinding;
		std::map<RendererPrivate::FMeshPassKey, std::shared_ptr<const RendererPrivate::FMeshPassPipeline>> Pipelines;
		// Receipts into frame-owned pass lists; records do not own duplicate command arrays.
		std::map<RendererPrivate::FMeshPassKey, FMeshDrawCommandHandle> Commands;
		std::map<RendererPrivate::FMeshPassKey, std::shared_ptr<const RendererPrivate::FMeshDrawCommand>> SelectedCachedCommands;
		std::map<RendererPrivate::FMeshPassKey, FMeshDrawCommandHandle> VisibleCommands;
		bool bReady = false;
	};

	template <typename... TBuckets>
	auto AssignResolvedIndices(TBuckets&... Buckets) -> uint32
	{
		uint32 NextIndex = 0;
		auto AssignBucket = [&NextIndex](auto& Bucket) {
			for (auto& Item : Bucket) Item.ResolvedIndex = NextIndex++;
		};
		(AssignBucket(Buckets), ...);
		return NextIndex;
	}
} // namespace Durin
