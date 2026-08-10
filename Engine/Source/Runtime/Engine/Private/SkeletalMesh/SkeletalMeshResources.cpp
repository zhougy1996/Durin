#include "SkeletalMesh/SkeletalMeshResources.h"

#include "DynamicRHI.h"
#include "Math/Operations.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		template<typename TResource>
		auto InitResource(TResource& Resource, FRHICommandListBase& CommandList) -> void
		{
			if (!Resource.IsInitialized()) Resource.InitResource(CommandList);
			else if constexpr (requires { Resource.GetRHI(); })
			{
				if (Resource.GetRHI() == nullptr) Resource.UpdateRHI(CommandList);
			}
			else if (!Resource.IsReady()) Resource.UpdateRHI(CommandList);
		}

		auto ReleaseResource(FRenderResource& Resource) -> void
		{
			if (Resource.IsInitialized()) Resource.ReleaseResource();
			else Resource.ReleaseRHI();
		}

		auto IsGeometryValid(const FSkeletalMeshRenderData& Data) -> bool
		{
			const size_t NumVertices =
				Data.VertexBuffers.Geometry.PositionVertexBuffer.GetPositions().size();
			const auto& StaticBuffer = Data.VertexBuffers.Geometry.StaticMeshVertexBuffer;
			if (NumVertices == 0 || StaticBuffer.TangentsVertexBuffer.GetNormals().size() != NumVertices
				|| StaticBuffer.TangentsVertexBuffer.GetTangents().size() != NumVertices
				|| Data.VertexBuffers.Geometry.ColorVertexBuffer.GetColors().size() != NumVertices
				|| Data.VertexBuffers.InfluenceVertexBuffer.GetInfluences().size() != NumVertices)
				return false;
			if (!std::ranges::all_of(StaticBuffer.TexCoordVertexBuffer.GetTexCoords(),
				[NumVertices](const auto& Channel) { return Channel.size() == NumVertices; }))
				return false;
			const auto& Indices = Data.IndexBuffer.GetIndices();
			if (Indices.empty() || Indices.size() % 3 != 0
				|| std::ranges::any_of(Indices,
					[NumVertices](uint32 Index) { return Index >= NumVertices; })) return false;
			if (Data.Sections.empty() || Data.MaterialSlots.empty()
				|| Data.PaletteBoneIndices.empty()
				|| Data.PaletteBoneIndices.size() != Data.InverseBindMatrices.size()
				|| Data.PaletteBoneIndices.size() != Data.InfluenceBounds.size()) return false;
			uint64 CoveredIndices = 0;
			for (const FSkeletalMeshRenderSection& Section : Data.Sections)
			{
				const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
				if (Section.FirstIndex != CoveredIndices || Section.IndexCount == 0
					|| Section.IndexCount % 3 != 0 || End > Indices.size()
					|| Section.MinVertexIndex > Section.MaxVertexIndex
					|| Section.MaxVertexIndex >= NumVertices
					|| Section.MaterialSlotIndex >= Data.MaterialSlots.size()) return false;
				CoveredIndices = End;
			}
			return CoveredIndices == Indices.size() && Data.LODIndex == 0
				&& Data.LocalBounds.bIsValid;
		}

		auto ToDoubleMatrix(const FMatrix4f& Source) -> FMatrix
		{
			FMatrix Result(0.0);
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result[Column][Row] = Source[Column][Row];
			return Result;
		}

		auto ContainsWithTolerance(const FBox& Bounds, const FVector3& Point) -> bool
		{
			constexpr double Tolerance = 1.0e-5;
			return Bounds.bIsValid && Point.x >= Bounds.Min.x - Tolerance
				&& Point.y >= Bounds.Min.y - Tolerance && Point.z >= Bounds.Min.z - Tolerance
				&& Point.x <= Bounds.Max.x + Tolerance && Point.y <= Bounds.Max.y + Tolerance
				&& Point.z <= Bounds.Max.z + Tolerance;
		}

		auto ValidateReferenceContainment(
			const FSkeletalMeshPayloadData& Payload,
			const DSkeleton& Skeleton,
			const FSkeletonTransform& MeshNodeBindTransform,
			std::span<const FBox> InfluenceBounds,
			std::string& OutError) -> bool
		{
			std::vector<FMatrix> ComponentMatrices(Skeleton.GetBoneCount(), FMatrix(1.0));
			const auto Bones = Skeleton.GetBones();
			for (size_t BoneIndex = 0; BoneIndex < Bones.size(); ++BoneIndex)
			{
				const FMatrix Local = ToDoubleMatrix(Bones[BoneIndex].ReferenceTransform.ToMatrix4f());
				ComponentMatrices[BoneIndex] = Bones[BoneIndex].ParentIndex >= 0
					? ComponentMatrices[static_cast<size_t>(Bones[BoneIndex].ParentIndex)] * Local
					: Local;
			}
			FMatrix InverseMesh;
			if (!Math::TryInverse(ToDoubleMatrix(MeshNodeBindTransform.ToMatrix4f()), InverseMesh))
			{
				OutError = "Skeletal-mesh bind transform is singular.";
				return false;
			}
			std::vector<FMatrix> Palette(Payload.PaletteBoneIndices.size());
			FBox ConservativeBounds;
			for (size_t PaletteIndex = 0; PaletteIndex < Palette.size(); ++PaletteIndex)
			{
				Palette[PaletteIndex] = InverseMesh
					* ComponentMatrices[Payload.PaletteBoneIndices[PaletteIndex]]
					* ToDoubleMatrix(Payload.InverseBindMatrices[PaletteIndex]);
				const FBox& Bound = InfluenceBounds[PaletteIndex];
				if (!Bound.bIsValid) continue;
				for (uint32 Corner = 0; Corner < 8; ++Corner)
				{
					const FVector3 Point(
						(Corner & 1u) ? Bound.Max.x : Bound.Min.x,
						(Corner & 2u) ? Bound.Max.y : Bound.Min.y,
						(Corner & 4u) ? Bound.Max.z : Bound.Min.z);
					ConservativeBounds.AddPoint(FVector3(Palette[PaletteIndex] * FVector4(Point, 1.0)));
				}
			}
			for (size_t VertexIndex = 0; VertexIndex < Payload.Positions.size(); ++VertexIndex)
			{
				FVector3 Skinned(0.0);
				const FSkeletalMeshVertexInfluences& Influences = Payload.Influences[VertexIndex];
				for (uint8 InfluenceIndex = 0; InfluenceIndex < Influences.Count; ++InfluenceIndex)
				{
					const auto PaletteIt = std::ranges::find(
						Payload.PaletteBoneIndices, Influences.BoneIndices[InfluenceIndex]);
					const size_t PaletteIndex = static_cast<size_t>(
						std::distance(Payload.PaletteBoneIndices.begin(), PaletteIt));
					Skinned += static_cast<double>(Influences.Weights[InfluenceIndex])
						* FVector3(Palette[PaletteIndex]
							* FVector4(FVector3(Payload.Positions[VertexIndex]), 1.0));
				}
				if (!Math::IsFinite(Skinned) || !ContainsWithTolerance(ConservativeBounds, Skinned))
				{
					OutError = "Skeletal-mesh influence bounds do not contain the reference-pose geometry.";
					return false;
				}
			}
			return true;
		}
	}

	auto FSkeletalMeshInfluenceVertexBuffer::Init(
		std::vector<FSkeletalMeshVertexInfluences> InInfluences,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Influences = std::move(InInfluences);
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FSkeletalMeshInfluenceVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Influences.empty() || GetRHI() != nullptr) return;
		std::vector<FSkeletalMeshInfluenceVertex> Vertices(Influences.size());
		for (size_t VertexIndex = 0; VertexIndex < Influences.size(); ++VertexIndex)
		{
			Vertices[VertexIndex].JointIndices = Influences[VertexIndex].BoneIndices;
			Vertices[VertexIndex].JointWeights = Influences[VertexIndex].Weights;
		}
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"SkeletalMeshInfluenceVertexBuffer",
			static_cast<uint32>(Vertices.size() * sizeof(Vertices.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = Vertices.data();
		Desc.InitialData.Size = static_cast<uint32>(Vertices.size() * sizeof(Vertices.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList), Desc));
	}

	auto FSkeletalMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> bool
	{
		check(IsInRenderingThread());
		if (!VertexBuffers.Geometry.PositionVertexBuffer.IsInitialized()
			&& !VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.IsInitialized()
			&& !VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.IsInitialized()
			&& !VertexBuffers.Geometry.ColorVertexBuffer.IsInitialized())
		{
			VertexBuffers.Geometry.Finalize(MaxStaticMeshUVChannels, true);
		}
		if (!IsGeometryValid(*this))
		{
			ReleaseResources();
			return false;
		}
		VertexBuffers.Geometry.InitResources(RHICmdList);
		InitResource(VertexBuffers.InfluenceVertexBuffer, RHICmdList);
		InitResource(IndexBuffer, RHICmdList);
		if (!VertexBuffers.IsReady() || !IndexBuffer.IsReady())
		{
			ReleaseResources();
			return false;
		}
		if (!VertexFactory.IsInitialized() && !VertexFactory.SetData(VertexBuffers))
		{
			ReleaseResources();
			return false;
		}
		InitResource(VertexFactory, RHICmdList);
		if (!VertexFactory.IsReady())
		{
			ReleaseResources();
			return false;
		}
		return true;
	}

	auto FSkeletalMeshRenderData::ReleaseResources() -> void
	{
		check(IsInRenderingThread());
		ReleaseResource(VertexFactory);
		ReleaseResource(IndexBuffer);
		ReleaseResource(VertexBuffers.InfluenceVertexBuffer);
		VertexBuffers.Geometry.ReleaseResources();
	}

	auto FSkeletalMeshRenderData::GetNumInitializedResources() const -> size_t
	{
		size_t Count = 0;
		auto Add = [&Count](const FRenderResource& Resource) {
			if (Resource.IsInitialized()) ++Count;
		};
		Add(VertexBuffers.Geometry.PositionVertexBuffer);
		Add(VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer);
		Add(VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer);
		Add(VertexBuffers.Geometry.ColorVertexBuffer);
		Add(VertexBuffers.InfluenceVertexBuffer);
		Add(IndexBuffer);
		Add(VertexFactory);
		return Count;
	}

	auto FSkeletalMeshRenderData::IsReadyForRendering() const -> bool
	{
		return IsGeometryValid(*this) && VertexBuffers.IsReady()
			&& IndexBuffer.IsReady() && VertexFactory.IsReady();
	}

#if DURIN_BUILD_DEBUG
	auto FSkeletalMeshRenderData::SetResourceDebugOwner(FName InOwner) -> void
	{
		VertexBuffers.Geometry.PositionVertexBuffer.SetDebugOwner(InOwner);
		VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.SetDebugOwner(InOwner);
		VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.SetDebugOwner(InOwner);
		VertexBuffers.Geometry.ColorVertexBuffer.SetDebugOwner(InOwner);
		VertexBuffers.InfluenceVertexBuffer.SetDebugOwner(InOwner);
		IndexBuffer.SetDebugOwner(InOwner);
		VertexFactory.SetDebugOwner(InOwner);
	}
#endif

	auto BuildSkeletalMeshRenderData(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		const FSkeletonTransform& MeshNodeBindTransform,
		std::span<const FSkeletalMeshMaterialSlotDefinition> MaterialSlots,
		std::unique_ptr<FSkeletalMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool
	{
		if (!MeshNodeBindTransform.IsValid(&OutError)) return false;
		if (!ValidateSkeletalMeshPayload(
			Payload, Skeleton, static_cast<uint32>(MaterialSlots.size()), OutError)) return false;
		auto Candidate = std::make_unique<FSkeletalMeshRenderData>();
		Candidate->VertexBuffers.Geometry.PositionVertexBuffer.Init(Payload.Positions);
		Candidate->VertexBuffers.Geometry.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			Payload.Normals, Payload.Tangents);
		uint8 NumTexCoords = 0;
		for (const auto& Channel : Payload.UVChannels)
			if (!Channel.empty()) ++NumTexCoords;
		Candidate->VertexBuffers.Geometry.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			Payload.UVChannels, static_cast<uint32>(Payload.Positions.size()), NumTexCoords);
		Candidate->VertexBuffers.Geometry.ColorVertexBuffer.Init(
			Payload.Colors, static_cast<uint32>(Payload.Positions.size()));
		Candidate->VertexBuffers.InfluenceVertexBuffer.Init(Payload.Influences);
		Candidate->IndexBuffer.Init(Payload.Indices);
		Candidate->Sections.reserve(Payload.Sections.size());
		for (const FSkeletalMeshSection& Section : Payload.Sections)
			Candidate->Sections.push_back({Section.Name, Section.FirstIndex, Section.IndexCount,
				Section.MinVertexIndex, Section.MaxVertexIndex, Section.MaterialSlotIndex,
				Section.LocalBounds});
		Candidate->MaterialSlots.reserve(MaterialSlots.size());
		for (const FSkeletalMeshMaterialSlotDefinition& Slot : MaterialSlots)
			Candidate->MaterialSlots.push_back(Slot.Name);
		Candidate->PaletteBoneIndices = Payload.PaletteBoneIndices;
		Candidate->InverseBindMatrices = Payload.InverseBindMatrices;
		Candidate->InfluenceBounds.resize(Payload.PaletteBoneIndices.size());
		for (size_t VertexIndex = 0; VertexIndex < Payload.Positions.size(); ++VertexIndex)
		{
			const FSkeletalMeshVertexInfluences& Vertex = Payload.Influences[VertexIndex];
			for (uint8 InfluenceIndex = 0; InfluenceIndex < Vertex.Count; ++InfluenceIndex)
			{
				const auto It = std::ranges::find(
					Payload.PaletteBoneIndices, Vertex.BoneIndices[InfluenceIndex]);
				check(It != Payload.PaletteBoneIndices.end());
				Candidate->InfluenceBounds[static_cast<size_t>(
					std::distance(Payload.PaletteBoneIndices.begin(), It))]
					.AddPoint(FVector3(Payload.Positions[VertexIndex]));
			}
		}
		Candidate->LocalBounds = Payload.LocalBounds;
		if (!ValidateReferenceContainment(Payload, Skeleton, MeshNodeBindTransform,
			Candidate->InfluenceBounds, OutError)) return false;
		if (!IsGeometryValid(*Candidate))
		{
			OutError = "Skeletal-mesh render-data conversion produced invalid geometry.";
			return false;
		}
		OutRenderData = std::move(Candidate);
		OutError.clear();
		return true;
	}
}
