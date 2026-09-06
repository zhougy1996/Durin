#include "StaticMesh/StaticMeshResources.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 StaticMeshRayQueryLeafTriangles = 8;

		struct FRayBuildCancelled {};
		struct FRayBuildControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 Work = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FRayBuildCancelled{};
			}
			auto Tick() -> void
			{
				if (++Work < 256) return;
				Work = 0;
				Check();
			}
		};

		struct FStaticMeshRayQueryBuildTriangle
		{
			uint32 Ordinal = 0;
			FBox Bounds;
			FVector3 Centroid{0.0};
		};

		auto UnionRayQueryBounds(const FBox& A, const FBox& B) -> FBox
		{
			if (!A.bIsValid) return B;
			if (!B.bIsValid) return A;
			return {Math::Min(A.Min, B.Min), Math::Max(A.Max, B.Max)};
		}

		auto BuildStaticMeshRayQueryRange(
			std::vector<FStaticMeshRayQueryBuildTriangle>& Triangles,
			size_t Begin, size_t End,
			FStaticMeshLODResources::FRayQueryAcceleration& Acceleration, FRayBuildControl& Control) -> uint32
		{
			Control.Check();
			const uint32 NodeIndex = static_cast<uint32>(Acceleration.Nodes.size());
			Acceleration.Nodes.push_back({});
			FBox Bounds;
			FBox CentroidBounds;
			for (size_t Index = Begin; Index < End; ++Index)
			{
				Control.Tick();
				Bounds = UnionRayQueryBounds(Bounds, Triangles[Index].Bounds);
				CentroidBounds.AddPoint(Triangles[Index].Centroid);
			}
			Acceleration.Nodes[NodeIndex].Bounds = Bounds;
			if (End - Begin <= StaticMeshRayQueryLeafTriangles)
			{
				auto& Node = Acceleration.Nodes[NodeIndex];
				Node.bLeaf = true;
				Node.First = static_cast<uint32>(Acceleration.TriangleOrdinals.size());
				Node.CountOrSecond = static_cast<uint32>(End - Begin);
				for (size_t Index = Begin; Index < End; ++Index)
					Acceleration.TriangleOrdinals.push_back(Triangles[Index].Ordinal);
				return NodeIndex;
			}
			const FVector3 Extent = CentroidBounds.Max - CentroidBounds.Min;
			uint32 Axis = Extent.y > Extent.x ? 1u : 0u;
			if (Extent.z > Extent[Axis]) Axis = 2u;
			std::stable_sort(Triangles.begin() + Begin, Triangles.begin() + End,
				[Axis, &Control](const FStaticMeshRayQueryBuildTriangle& A,
					const FStaticMeshRayQueryBuildTriangle& B) {
					Control.Tick();
					return A.Centroid[Axis] < B.Centroid[Axis]
						|| (A.Centroid[Axis] == B.Centroid[Axis] && A.Ordinal < B.Ordinal);
				});
			const size_t Middle = Begin + (End - Begin) / 2;
			const uint32 Left = BuildStaticMeshRayQueryRange(Triangles, Begin, Middle, Acceleration, Control);
			const uint32 Right = BuildStaticMeshRayQueryRange(Triangles, Middle, End, Acceleration, Control);
			Acceleration.Nodes[NodeIndex].First = Left;
			Acceleration.Nodes[NodeIndex].CountOrSecond = Right;
			return NodeIndex;
		}

		auto CountStaticMeshRayQueryNodes(size_t TriangleCount, FRayBuildControl& Control) -> size_t
		{
			Control.Tick();
			if (TriangleCount <= StaticMeshRayQueryLeafTriangles) return 1;
			const size_t Left = TriangleCount / 2;
			return 1 + CountStaticMeshRayQueryNodes(Left, Control)
				+ CountStaticMeshRayQueryNodes(TriangleCount - Left, Control);
		}
	}

	static auto BuildRayQueryAcceleration(const FStaticMeshLODResources& LOD, FRayBuildControl& Control)
		-> std::shared_ptr<const FStaticMeshLODResources::FRayQueryAcceleration>
	{
		Control.Check();
		const auto BuildStart = std::chrono::steady_clock::now();
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0
			|| Positions.size() > std::numeric_limits<uint32>::max()
			|| Indices.size() > std::numeric_limits<uint32>::max()) return nullptr;
		const size_t TriangleCount = Indices.size() / 3;
		const size_t NodeCount = CountStaticMeshRayQueryNodes(TriangleCount, Control);
		const uint64 RetainedBytes = sizeof(FStaticMeshLODResources::FRayQueryAcceleration)
			+ NodeCount * sizeof(FStaticMeshLODResources::FRayQueryNode) + TriangleCount * sizeof(uint32);
		if (RetainedBytes > MaximumStaticMeshRayQueryAccelerationBytes
			|| RetainedBytes > std::max<uint64>(1024, TriangleCount * 96ull)) return nullptr;
		std::vector<FStaticMeshRayQueryBuildTriangle> Triangles;
		Triangles.reserve(Indices.size() / 3);
		for (uint32 Index = 0; Index < Indices.size(); Index += 3)
		{
			Control.Tick();
			const uint32 I0 = Indices[Index];
			const uint32 I1 = Indices[Index + 1];
			const uint32 I2 = Indices[Index + 2];
			if (I0 >= Positions.size() || I1 >= Positions.size() || I2 >= Positions.size())
				return nullptr;
			const FVector3 A(Positions[I0]);
			const FVector3 B(Positions[I1]);
			const FVector3 C(Positions[I2]);
			if (!Math::IsFinite(A) || !Math::IsFinite(B) || !Math::IsFinite(C)) return nullptr;
			FBox Bounds;
			Bounds.AddPoint(A);
			Bounds.AddPoint(B);
			Bounds.AddPoint(C);
			Triangles.push_back({Index / 3, Bounds, (A + B + C) / 3.0});
		}
		auto Acceleration = std::make_shared<FStaticMeshLODResources::FRayQueryAcceleration>();
		Acceleration->SourceVertexCount = static_cast<uint32>(Positions.size());
		Acceleration->SourceIndexCount = static_cast<uint32>(Indices.size());
		Acceleration->Nodes.reserve(NodeCount);
		Acceleration->TriangleOrdinals.reserve(Triangles.size());
		BuildStaticMeshRayQueryRange(Triangles, 0, Triangles.size(), *Acceleration, Control);
		Acceleration->RetainedBytes = sizeof(FStaticMeshLODResources::FRayQueryAcceleration)
			+ Acceleration->Nodes.capacity() * sizeof(FStaticMeshLODResources::FRayQueryNode)
			+ Acceleration->TriangleOrdinals.capacity() * sizeof(uint32);
		Acceleration->BuildNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - BuildStart).count());
		const uint64 LayoutBudget = std::max<uint64>(1024, Triangles.size() * 96ull);
		if (Acceleration->RetainedBytes > MaximumStaticMeshRayQueryAccelerationBytes
			|| Acceleration->RetainedBytes > LayoutBudget) return nullptr;
		Control.Check();
		return Acceleration;
	}

	auto BuildStaticMeshRayQueryAcceleration(const FStaticMeshLODResources& LOD,
		const std::function<bool()>& ShouldCancel)
		-> std::shared_ptr<const FStaticMeshLODResources::FRayQueryAcceleration>
	{
		FRayBuildControl Control{ShouldCancel};
		try { return BuildRayQueryAcceleration(LOD, Control); }
		catch (const FRayBuildCancelled&) { return nullptr; }
	}
}
