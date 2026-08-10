#pragma once

#include "EngineAPI.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshVertexFactory.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	struct FSkeletalMeshInfluenceVertex
	{
		std::array<uint16, MaximumSkeletalMeshInfluences> JointIndices{};
		std::array<float, MaximumSkeletalMeshInfluences> JointWeights{};
	};

	static_assert(sizeof(FSkeletalMeshInfluenceVertex) == 24);

	class FSkeletalMeshInfluenceVertexBuffer : public FVertexBuffer
	{
	public:
		ENGINE_API auto Init(
			std::vector<FSkeletalMeshVertexInfluences> InInfluences,
			bool bInNeedsCPUAccess = true) -> void;
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FSkeletalMeshInfluenceVertexBuffer";
		}
		auto GetNumVertices() const -> uint32
		{
			return static_cast<uint32>(Influences.size());
		}
		auto GetStride() const -> uint32 { return sizeof(FSkeletalMeshInfluenceVertex); }
		auto NeedsCPUAccess() const -> bool { return bNeedsCPUAccess; }
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0 && GetRHI() != nullptr;
		}
		auto GetInfluences() const -> const std::vector<FSkeletalMeshVertexInfluences>&
		{
			return Influences;
		}

	private:
		std::vector<FSkeletalMeshVertexInfluences> Influences;
		bool bNeedsCPUAccess = true;
	};

	struct FSkeletalMeshVertexBuffers
	{
		FStaticMeshVertexBuffers Geometry;
		FSkeletalMeshInfluenceVertexBuffer InfluenceVertexBuffer;

		auto IsReady() const -> bool
		{
			return Geometry.IsReady() && InfluenceVertexBuffer.IsReady();
		}
	};

	struct FSkeletalMeshRenderSection
	{
		FName Name;
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;
	};

	struct FSkeletalMeshRenderData
	{
		FSkeletalMeshVertexBuffers VertexBuffers;
		FRawStaticIndexBuffer IndexBuffer;
		std::vector<FSkeletalMeshRenderSection> Sections;
		std::vector<FName> MaterialSlots;
		std::vector<uint16> PaletteBoneIndices;
		std::vector<FMatrix4f> InverseBindMatrices;
		std::vector<FBox> InfluenceBounds;
		FBox LocalBounds;
		FSkeletalMeshVertexFactory VertexFactory;
		uint32 LODIndex = 0;

		ENGINE_API auto InitResources(FRHICommandListImmediate& RHICmdList) -> bool;
		ENGINE_API auto ReleaseResources() -> void;
		ENGINE_API auto GetNumInitializedResources() const -> size_t;
		ENGINE_API auto IsReadyForRendering() const -> bool;
#if DURIN_BUILD_DEBUG
		ENGINE_API auto SetResourceDebugOwner(FName InOwner) -> void;
#endif
	};

	ENGINE_API auto BuildSkeletalMeshRenderData(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		const FSkeletonTransform& MeshNodeBindTransform,
		std::span<const FSkeletalMeshMaterialSlotDefinition> MaterialSlots,
		std::unique_ptr<FSkeletalMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool;
}
