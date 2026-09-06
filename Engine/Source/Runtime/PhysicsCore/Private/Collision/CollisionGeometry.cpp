#include "Collision/CollisionGeometry.h"
#include "Physics/PhysicsTypes.h"

namespace Durin
{
	class FCollisionGeometry
	{
	public:
		uint64 Identity = 0;
		std::vector<FCollisionGeometryChild> Children;
		FVector3 LocalMin{0.0};
		FVector3 LocalMax{0.0};
		uint64 RetainedBytes = 0;
	};

	class FFeatureCollisionGeometry final : public FCollisionGeometry
	{
	public:
		ECollisionGeometryKind Kind = ECollisionGeometryKind::TriangleMesh;
		std::vector<FVector3> Vertices;
		std::vector<FCollisionGeometryTriangle> Triangles;
		std::vector<FCollisionGeometryNode> Nodes;
		std::vector<uint32> LeafTriangles;
		std::vector<FCollisionHullPlane> HullPlanes;
		std::vector<FCollisionHullHalfEdge> HullHalfEdges;
		std::vector<FCollisionHullFace> HullFaces;
	};

	static_assert(sizeof(FCollisionGeometryNode) == 32);
	static_assert(sizeof(FCollisionHullPlane) == 16);
	static_assert(sizeof(FCollisionHullHalfEdge) == 16);
	static_assert(sizeof(FCollisionHullFace) == 16);

	namespace
	{
		std::atomic<uint64> GNextCollisionGeometryIdentity = 1;
		struct FCollisionBuildCancelled {};

		// Local stack unwinding lets nested BVH partitions abandon all scratch ownership.
		struct FCollisionBuildControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 WorkSinceCheckpoint = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FCollisionBuildCancelled{};
			}
			auto Tick() -> void
			{
				if (++WorkSinceCheckpoint < 256) return;
				WorkSinceCheckpoint = 0;
				Check();
			}
		};

		constexpr uint32 MaximumConvexHullVertices = 256;
		constexpr uint32 MaximumCollisionTriangles = 2'000'000;
		constexpr double FeatureTolerance = 1.0e-10;

		auto AllocateCollisionGeometryIdentity(ECollisionGeometryKind Kind) -> uint64
		{
			uint64 Sequence = GNextCollisionGeometryIdentity.fetch_add(1, std::memory_order_relaxed);
			if (Sequence == 0) Sequence = GNextCollisionGeometryIdentity.fetch_add(1, std::memory_order_relaxed);
			return (Sequence << 3) | static_cast<uint8>(Kind);
		}

		auto GetEncodedGeometryKind(const FCollisionGeometry* Payload) -> ECollisionGeometryKind
		{
			return Payload ? static_cast<ECollisionGeometryKind>(Payload->Identity & 0x7u)
				: ECollisionGeometryKind::Primitive;
		}



		auto GetFeaturePayload(const FCollisionGeometry* Payload) -> const FFeatureCollisionGeometry*
		{
			const ECollisionGeometryKind Kind = GetEncodedGeometryKind(Payload);
			return Payload && (Kind == ECollisionGeometryKind::ConvexHull
				|| Kind == ECollisionGeometryKind::TriangleMesh)
				? static_cast<const FFeatureCollisionGeometry*>(Payload) : nullptr;
		}


		auto ValidateFeatureInput(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			std::span<const uint32> SourceOrdinals, FCollisionBuildControl& Control) -> bool
		{
			if (Vertices.empty() || Indices.empty() || Indices.size() % 3 != 0) return false;
			const size_t TriangleCount = Indices.size() / 3;
			if (TriangleCount > MaximumCollisionTriangles
				|| (!SourceOrdinals.empty() && SourceOrdinals.size() != TriangleCount)) return false;
			for (const FVector3& Vertex : Vertices)
			{
				Control.Tick();
				if (!Math::IsFinite(Vertex)) return false;
			}
			for (uint32 Index : Indices)
			{
				Control.Tick();
				if (Index >= Vertices.size()) return false;
			}
			for (size_t Triangle = 0; Triangle < TriangleCount; ++Triangle)
			{
				Control.Tick();
				const uint32 A = Indices[Triangle * 3];
				const uint32 B = Indices[Triangle * 3 + 1];
				const uint32 C = Indices[Triangle * 3 + 2];
				if (A == B || B == C || C == A
					|| Math::LengthSquared(Math::Cross(Vertices[B] - Vertices[A], Vertices[C] - Vertices[A]))
						<= FeatureTolerance * FeatureTolerance) return false;
			}
			return true;
		}

		auto ValidateConvexHull(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices, FCollisionBuildControl& Control) -> bool
		{
			if (Vertices.size() < 4 || Vertices.size() > MaximumConvexHullVertices
				|| Indices.size() < 12 || !ValidateFeatureInput(Vertices, Indices, {}, Control)) return false;
			std::map<std::pair<uint32, uint32>, std::pair<uint32, int32>> Edges;
			double SignedVolume = 0.0;
			for (size_t Offset = 0; Offset < Indices.size(); Offset += 3)
			{
				Control.Tick();
				const uint32 A = Indices[Offset];
				const uint32 B = Indices[Offset + 1];
				const uint32 C = Indices[Offset + 2];
				SignedVolume += Math::Dot(Vertices[A], Math::Cross(Vertices[B], Vertices[C]));
				for (const auto [From, To] : {std::pair{A, B}, std::pair{B, C}, std::pair{C, A}})
				{
					Control.Tick();
					const auto Key = std::minmax(From, To);
					auto& Edge = Edges[{Key.first, Key.second}];
					++Edge.first;
					Edge.second += From < To ? 1 : -1;
				}
			}
			if (std::abs(SignedVolume) <= FeatureTolerance) return false;
			for (const auto& [Key, Edge] : Edges)
			{
				Control.Tick();
				if (Edge.first != 2 || Edge.second != 0) return false;
			}

			const double Orientation = SignedVolume > 0.0 ? 1.0 : -1.0;
			for (size_t Offset = 0; Offset < Indices.size(); Offset += 3)
			{
				Control.Tick();
				const FVector3& A = Vertices[Indices[Offset]];
				const FVector3 Normal = Math::Cross(
					Vertices[Indices[Offset + 1]] - A, Vertices[Indices[Offset + 2]] - A) * Orientation;
				for (const FVector3& Vertex : Vertices)
				{
					Control.Tick();
					if (Math::Dot(Normal, Vertex - A) > FeatureTolerance) return false;
				}
			}
			return true;
		}

		auto MakeFeatureGeometry(
			ECollisionGeometryKind Kind,
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			std::span<const uint32> SourceOrdinals,
			std::span<const FCollisionGeometryNode> Nodes,
			std::span<const uint32> LeafTriangles, FCollisionBuildControl& Control) -> std::shared_ptr<const FCollisionGeometry>
		{
			auto Payload = std::make_shared<FFeatureCollisionGeometry>();
			Payload->Kind = Kind;
			Control.Check();
			Payload->Vertices.assign(Vertices.begin(), Vertices.end());
			Payload->Triangles.reserve(Indices.size() / 3);
			Control.Check();
			Payload->Nodes.assign(Nodes.begin(), Nodes.end());
			Control.Check();
			Payload->LeafTriangles.assign(LeafTriangles.begin(), LeafTriangles.end());
			for (size_t Triangle = 0; Triangle < Indices.size() / 3; ++Triangle)
			{
				Control.Tick();
				Payload->Triangles.push_back({Indices[Triangle * 3], Indices[Triangle * 3 + 1],
					Indices[Triangle * 3 + 2], SourceOrdinals.empty()
						? static_cast<uint32>(Triangle) : SourceOrdinals[Triangle]});
			}
			if (Kind == ECollisionGeometryKind::ConvexHull)
			{
				Payload->HullPlanes.reserve(Payload->Triangles.size());
				Payload->HullFaces.reserve(Payload->Triangles.size());
				Payload->HullHalfEdges.resize(Payload->Triangles.size() * 3);
				std::map<std::pair<uint32, uint32>, uint32> DirectedEdges;
				for (uint32 Triangle = 0; Triangle < Payload->Triangles.size(); ++Triangle)
				{
					Control.Tick();
					const FCollisionGeometryTriangle& Face = Payload->Triangles[Triangle];
					const FVector3 Normal = Math::Normalize(Math::Cross(
						Payload->Vertices[Face.Second] - Payload->Vertices[Face.First],
						Payload->Vertices[Face.Third] - Payload->Vertices[Face.First]));
					Payload->HullPlanes.push_back({FVector3f(Normal),
						static_cast<float>(Math::Dot(Normal, Payload->Vertices[Face.First]))});
					const uint32 FirstEdge = Triangle * 3;
					Payload->HullFaces.push_back({FirstEdge, 3, Face.SourceOrdinal, 0});
					const std::array<uint32, 3> FaceVertices{Face.First, Face.Second, Face.Third};
					for (uint32 Edge = 0; Edge < 3; ++Edge)
					{
						Control.Tick();
						const uint32 Index = FirstEdge + Edge;
						const uint32 From = FaceVertices[Edge];
						const uint32 To = FaceVertices[(Edge + 1) % 3];
						Payload->HullHalfEdges[Index] = {From, std::numeric_limits<uint32>::max(),
							FirstEdge + (Edge + 1) % 3, Triangle};
						DirectedEdges[{From, To}] = Index;
					}
				}
				for (const auto& [Directed, Index] : DirectedEdges)
					Payload->HullHalfEdges[Index].Twin = DirectedEdges.at({Directed.second, Directed.first});
			}
			Payload->LocalMin = Payload->Vertices.front();
			Payload->LocalMax = Payload->Vertices.front();
			for (const FVector3& Vertex : Payload->Vertices)
			{
				Control.Tick();
				Payload->LocalMin = Math::Min(Payload->LocalMin, Vertex);
				Payload->LocalMax = Math::Max(Payload->LocalMax, Vertex);
			}
			Payload->Identity = AllocateCollisionGeometryIdentity(Kind);
			Payload->RetainedBytes = sizeof(FFeatureCollisionGeometry)
				+ Payload->Vertices.capacity() * sizeof(FVector3)
				+ Payload->Triangles.capacity() * sizeof(FCollisionGeometryTriangle)
				+ Payload->Nodes.capacity() * sizeof(FCollisionGeometryNode)
				+ Payload->LeafTriangles.capacity() * sizeof(uint32)
				+ Payload->HullPlanes.capacity() * sizeof(FCollisionHullPlane)
				+ Payload->HullHalfEdges.capacity() * sizeof(FCollisionHullHalfEdge)
				+ Payload->HullFaces.capacity() * sizeof(FCollisionHullFace);
			Control.Check();
			return Payload;
		}

		auto BuildGeometryChildBounds(
			const FCollisionGeometryChild& Child, FVector3& OutMin, FVector3& OutMax) -> bool
		{
			if (!Child.Shape.IsValid() || !IsValidPhysicsTransform(Child.LocalTransform)) return false;
			FVector3 LocalExtent;
			switch (Child.Shape.GetType())
			{
			case ECollisionShapeType::Box:
				LocalExtent = Child.Shape.GetBoxHalfExtent() * Child.LocalTransform.Scale3D;
				break;
			case ECollisionShapeType::Sphere:
			{
				const double Radius = Child.Shape.GetSphereRadius() * std::max({
					Child.LocalTransform.Scale3D.x,
					Child.LocalTransform.Scale3D.y,
					Child.LocalTransform.Scale3D.z});
				LocalExtent = FVector3(Radius);
				break;
			}
			case ECollisionShapeType::Capsule:
			{
				const double Radius = Child.Shape.GetCapsuleRadius()
					* std::max(Child.LocalTransform.Scale3D.x, Child.LocalTransform.Scale3D.y);
				const double HalfHeight = std::max(
					Radius, Child.Shape.GetCapsuleHalfHeight() * Child.LocalTransform.Scale3D.z);
				LocalExtent = FVector3(Radius, Radius, HalfHeight);
				break;
			}
			}
			FQuat Rotation;
			if (!Math::TryNormalize(Child.LocalTransform.Rotation, Rotation)) return false;
			const FVector3 X = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Forward));
			const FVector3 Y = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Right));
			const FVector3 Z = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Up));
			const FVector3 WorldExtent = X * LocalExtent.x + Y * LocalExtent.y + Z * LocalExtent.z;
			OutMin = Child.LocalTransform.Translation - WorldExtent;
			OutMax = Child.LocalTransform.Translation + WorldExtent;
			return Math::IsFinite(OutMin) && Math::IsFinite(OutMax);
		}

		struct FTriangleBuildRecord
		{
			uint32 Triangle = 0;
			uint32 Ordinal = 0;
			FVector3 Minimum{0.0};
			FVector3 Maximum{0.0};
			FVector3 Centroid{0.0};
		};

		auto OutwardMinimum(double Value) -> float
		{
			return std::nextafter(static_cast<float>(Value), -std::numeric_limits<float>::infinity());
		}

		auto OutwardMaximum(double Value) -> float
		{
			return std::nextafter(static_cast<float>(Value), std::numeric_limits<float>::infinity());
		}

		auto BuildDeterministicBvh(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			std::span<const uint32> Ordinals,
			std::vector<FCollisionGeometryNode>& OutNodes,
			std::vector<uint32>& OutLeafTriangles,
			uint32& OutMaximumDepth, FCollisionBuildControl& Control) -> bool
		{
			std::vector<FTriangleBuildRecord> Records;
			Records.reserve(Indices.size() / 3);
			for (uint32 Triangle = 0; Triangle < Indices.size() / 3; ++Triangle)
			{
				Control.Tick();
				const FVector3& A = Vertices[Indices[Triangle * 3]];
				const FVector3& B = Vertices[Indices[Triangle * 3 + 1]];
				const FVector3& C = Vertices[Indices[Triangle * 3 + 2]];
				Records.push_back({Triangle, Ordinals[Triangle],
					Math::Min(A, Math::Min(B, C)), Math::Max(A, Math::Max(B, C)), (A + B + C) / 3.0});
			}
			OutNodes.clear();
			OutLeafTriangles.clear();
			OutNodes.reserve(Records.size() * 2);
			OutLeafTriangles.reserve(Records.size());
			OutMaximumDepth = 0;
			bool bDepthExceeded = false;
			std::function<uint32(size_t, size_t, uint32)> Build =
				[&](size_t First, size_t Count, uint32 Depth) -> uint32
			{
					Control.Check();
					OutMaximumDepth = std::max(OutMaximumDepth, Depth);
					const uint32 NodeIndex = static_cast<uint32>(OutNodes.size());
					OutNodes.emplace_back();
					FVector3 Minimum = Records[First].Minimum;
					FVector3 Maximum = Records[First].Maximum;
					FVector3 CentroidMinimum = Records[First].Centroid;
					FVector3 CentroidMaximum = Records[First].Centroid;
					for (size_t Offset = 1; Offset < Count; ++Offset)
					{
						Control.Tick();
						const FTriangleBuildRecord& Record = Records[First + Offset];
						Minimum = Math::Min(Minimum, Record.Minimum);
						Maximum = Math::Max(Maximum, Record.Maximum);
						CentroidMinimum = Math::Min(CentroidMinimum, Record.Centroid);
						CentroidMaximum = Math::Max(CentroidMaximum, Record.Centroid);
					}
					auto WriteBounds = [&](FCollisionGeometryNode& Node) {
						for (uint32 Axis = 0; Axis < 3; ++Axis)
						{
							Control.Tick();
							Node.Minimum[Axis] = OutwardMinimum(Minimum[Axis]);
							Node.Maximum[Axis] = OutwardMaximum(Maximum[Axis]);
						}
					};
					if (Count <= 8)
					{
						FCollisionGeometryNode& Node = OutNodes[NodeIndex];
						WriteBounds(Node);
						Node.First = static_cast<uint32>(OutLeafTriangles.size());
						Node.CountOrSecond = 0x80000000u | static_cast<uint32>(Count);
						for (size_t Offset = 0; Offset < Count; ++Offset)
							OutLeafTriangles.push_back(Records[First + Offset].Triangle);
						return NodeIndex;
					}
					if (Depth >= 64)
					{
						bDepthExceeded = true;
						return NodeIndex;
					}
					const FVector3 Extent = CentroidMaximum - CentroidMinimum;
					uint32 Axis = Extent.y > Extent.x ? 1u : 0u;
					if (Extent.z > Extent[Axis]) Axis = 2u;
					std::stable_sort(Records.begin() + First, Records.begin() + First + Count,
						[Axis, &Control](const FTriangleBuildRecord& Left, const FTriangleBuildRecord& Right) {
							Control.Tick();
							if (Left.Centroid[Axis] != Right.Centroid[Axis])
								return Left.Centroid[Axis] < Right.Centroid[Axis];
							if (Left.Ordinal != Right.Ordinal) return Left.Ordinal < Right.Ordinal;
							return Left.Triangle < Right.Triangle;
						});
					const size_t LeftCount = Count / 2;
					const uint32 Left = Build(First, LeftCount, Depth + 1);
					const uint32 Right = Build(First + LeftCount, Count - LeftCount, Depth + 1);
					FCollisionGeometryNode& Node = OutNodes[NodeIndex];
					WriteBounds(Node);
					Node.First = Left;
					Node.CountOrSecond = Right;
					return NodeIndex;
				};
			Build(0, Records.size(), 1);
			return !bDepthExceeded;
		}
	}

	auto FCollisionGeometryRef::MakePrimitive(const FCollisionShape& Shape) -> FCollisionGeometryRef
	{
		if (!Shape.IsValid()) return {};
		const FCollisionGeometryChild Child{.Shape = Shape, .LocalTransform = FTransform()};
		return MakeCompound(std::span(&Child, 1));
	}

	auto FCollisionGeometryRef::MakeCompound(std::span<const FCollisionGeometryChild> Children)
		-> FCollisionGeometryRef
	{
		if (Children.empty() || Children.size() > 64) return {};
		auto Payload = std::make_shared<FCollisionGeometry>();
		Payload->Children.reserve(Children.size());
		for (const FCollisionGeometryChild& Child : Children)
		{
			FVector3 ChildMin;
			FVector3 ChildMax;
			if (!BuildGeometryChildBounds(Child, ChildMin, ChildMax)) return {};
			if (Payload->Children.empty())
			{
				Payload->LocalMin = ChildMin;
				Payload->LocalMax = ChildMax;
			}
			else
			{
				Payload->LocalMin = Math::Min(Payload->LocalMin, ChildMin);
				Payload->LocalMax = Math::Max(Payload->LocalMax, ChildMax);
			}
			Payload->Children.push_back(Child);
		}
		Payload->Identity = AllocateCollisionGeometryIdentity(Children.size() > 1
			? ECollisionGeometryKind::Compound : ECollisionGeometryKind::Primitive);
		Payload->RetainedBytes = sizeof(FCollisionGeometry)
			+ Payload->Children.capacity() * sizeof(FCollisionGeometryChild);
		return FCollisionGeometryRef(std::move(Payload));
	}

	auto FCollisionGeometryRef::MakeConvexHull(
		std::span<const FVector3> Vertices,
		std::span<const uint32> Indices,
		const std::function<bool()>& ShouldCancel) -> FCollisionGeometryRef
	try
	{
		FCollisionBuildControl Control{ShouldCancel};
		Control.Check();
		if (!ValidateConvexHull(Vertices, Indices, Control)) return {};
		std::vector<uint32> OutwardIndices(Indices.begin(), Indices.end());
		double SignedVolume = 0.0;
		for (size_t Offset = 0; Offset < Indices.size(); Offset += 3)
			SignedVolume += Math::Dot(Vertices[Indices[Offset]],
				Math::Cross(Vertices[Indices[Offset + 1]], Vertices[Indices[Offset + 2]]));
		if (SignedVolume < 0.0)
			for (size_t Offset = 0; Offset < OutwardIndices.size(); Offset += 3)
				std::swap(OutwardIndices[Offset + 1], OutwardIndices[Offset + 2]);
		return FCollisionGeometryRef(MakeFeatureGeometry(
			ECollisionGeometryKind::ConvexHull, Vertices, OutwardIndices, {}, {}, {}, Control));
	}
	catch (const FCollisionBuildCancelled&)
	{
		return {};
	}

	auto FCollisionGeometryRef::BuildConvexHull(
		std::span<const FVector3> Points,
		FCollisionGeometryBuildDiagnostics* Diagnostics,
		const std::function<bool()>& ShouldCancel) -> FCollisionGeometryRef
	{
		FCollisionGeometryBuildDiagnostics Result;
		Result.SourceVertices = Points.size() <= std::numeric_limits<uint32>::max()
			? static_cast<uint32>(Points.size()) : std::numeric_limits<uint32>::max();
		auto Finish = [&](FCollisionGeometryBuildDiagnostics Value) {
			if (Diagnostics) *Diagnostics = Value;
		};
		if (Points.size() < 4)
		{
			Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
			Finish(Result);
			return {};
		}
		if (Points.size() > MaximumConvexHullVertices)
		{
			Result.Status = ECollisionGeometryBuildStatus::LimitExceeded;
			Finish(Result);
			return {};
		}
		try
		{
			FCollisionBuildControl Control{ShouldCancel};
			Control.Check();
			std::vector<FVector3> Vertices(Points.begin(), Points.end());
			for (const FVector3& Point : Vertices)
			{
				Control.Tick();
				if (!Math::IsFinite(Point))
				{
					Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
					Finish(Result);
					return {};
				}
			}
			std::ranges::sort(Vertices, [](const FVector3& Left, const FVector3& Right) {
				if (Left.x != Right.x) return Left.x < Right.x;
				if (Left.y != Right.y) return Left.y < Right.y;
				return Left.z < Right.z;
			});
			Vertices.erase(std::unique(Vertices.begin(), Vertices.end()), Vertices.end());
			if (Vertices.size() < 4)
			{
				Result.Status = ECollisionGeometryBuildStatus::EmptyAfterCleanup;
				Finish(Result);
				return {};
			}
			const uint32 First = 0;
			uint32 Second = 1;
			double BestDistance = 0.0;
			for (uint32 Index = 1; Index < Vertices.size(); ++Index)
			{
				Control.Tick();
				const double Distance = Math::LengthSquared(Vertices[Index] - Vertices[First]);
				if (Distance > BestDistance)
				{
					BestDistance = Distance;
					Second = Index;
				}
			}
			uint32 Third = 0;
			BestDistance = 0.0;
			const FVector3 Line = Vertices[Second] - Vertices[First];
			for (uint32 Index = 0; Index < Vertices.size(); ++Index)
			{
				Control.Tick();
				const double Distance = Math::LengthSquared(Math::Cross(Line, Vertices[Index] - Vertices[First]));
				if (Distance > BestDistance)
				{
					BestDistance = Distance;
					Third = Index;
				}
			}
			if (BestDistance <= FeatureTolerance * FeatureTolerance)
			{
				Result.Status = ECollisionGeometryBuildStatus::EmptyAfterCleanup;
				Finish(Result);
				return {};
			}
			uint32 Fourth = 0;
			BestDistance = 0.0;
			const FVector3 PlaneNormal = Math::Cross(
				Vertices[Second] - Vertices[First], Vertices[Third] - Vertices[First]);
			for (uint32 Index = 0; Index < Vertices.size(); ++Index)
			{
				Control.Tick();
				const double Distance = std::abs(Math::Dot(PlaneNormal, Vertices[Index] - Vertices[First]));
				if (Distance > BestDistance)
				{
					BestDistance = Distance;
					Fourth = Index;
				}
			}
			if (BestDistance <= FeatureTolerance)
			{
				Result.Status = ECollisionGeometryBuildStatus::EmptyAfterCleanup;
				Finish(Result);
				return {};
			}

			struct FFace { uint32 A; uint32 B; uint32 C; bool bDeleted = false; };
			std::vector<FFace> Faces{
				{First, Second, Third}, {First, Fourth, Second},
				{First, Third, Fourth}, {Second, Fourth, Third}};
			const FVector3 Interior = (Vertices[First] + Vertices[Second]
				+ Vertices[Third] + Vertices[Fourth]) / 4.0;
			auto Orient = [&](FFace& Face) {
				const FVector3 Normal = Math::Cross(
					Vertices[Face.B] - Vertices[Face.A], Vertices[Face.C] - Vertices[Face.A]);
				if (Math::Dot(Normal, Interior - Vertices[Face.A]) > 0.0) std::swap(Face.B, Face.C);
			};
			for (FFace& Face : Faces) Orient(Face);
			const std::set<uint32> Seed{First, Second, Third, Fourth};
			for (uint32 Point = 0; Point < Vertices.size(); ++Point)
			{
				Control.Tick();
				if (Seed.contains(Point)) continue;
				std::vector<uint32> Visible;
				for (uint32 FaceIndex = 0; FaceIndex < Faces.size(); ++FaceIndex)
				{
					Control.Tick();
					const FFace& Face = Faces[FaceIndex];
					if (Face.bDeleted) continue;
					const FVector3 Normal = Math::Cross(
						Vertices[Face.B] - Vertices[Face.A], Vertices[Face.C] - Vertices[Face.A]);
					if (Math::Dot(Normal, Vertices[Point] - Vertices[Face.A]) > FeatureTolerance)
						Visible.push_back(FaceIndex);
				}
				if (Visible.empty()) continue;
				struct FHorizonEdge { uint32 From = 0; uint32 To = 0; uint32 Count = 0; };
				std::map<std::pair<uint32, uint32>, FHorizonEdge> Horizon;
				for (uint32 FaceIndex : Visible)
				{
					Control.Tick();
					FFace& Face = Faces[FaceIndex];
					for (const auto [From, To] : {std::pair{Face.A, Face.B},
						std::pair{Face.B, Face.C}, std::pair{Face.C, Face.A}})
					{
						const auto Ordered = std::minmax(From, To);
						auto& Edge = Horizon[{Ordered.first, Ordered.second}];
						if (Edge.Count++ == 0) { Edge.From = From; Edge.To = To; }
					}
					Face.bDeleted = true;
				}
				for (const auto& [Key, Edge] : Horizon)
				{
					Control.Tick();
					if (Edge.Count != 1) continue;
					FFace Face{Edge.To, Edge.From, Point};
					Orient(Face);
					Faces.push_back(Face);
				}
			}
			std::vector<FFace> FinalFaces;
			for (const FFace& Face : Faces) if (!Face.bDeleted) FinalFaces.push_back(Face);
			std::ranges::sort(FinalFaces, [](const FFace& Left, const FFace& Right) {
				return std::tuple{Left.A, Left.B, Left.C} < std::tuple{Right.A, Right.B, Right.C};
			});
			std::set<uint32> Used;
			for (const FFace& Face : FinalFaces) Used.insert({Face.A, Face.B, Face.C});
			std::vector<FVector3> HullVertices;
			std::vector<uint32> Remap(Vertices.size(), std::numeric_limits<uint32>::max());
			for (uint32 Old : Used)
			{
				Control.Tick();
				Remap[Old] = static_cast<uint32>(HullVertices.size());
				HullVertices.push_back(Vertices[Old]);
			}
			std::vector<uint32> HullIndices;
			HullIndices.reserve(FinalFaces.size() * 3);
			for (const FFace& Face : FinalFaces)
				HullIndices.insert(HullIndices.end(), {Remap[Face.A], Remap[Face.B], Remap[Face.C]});
			bool bFinalizationCancelled = false;
			FCollisionGeometryRef Geometry = MakeConvexHull(HullVertices, HullIndices, [&] {
				bFinalizationCancelled = bFinalizationCancelled || (ShouldCancel && ShouldCancel());
				return bFinalizationCancelled;
			});
			if (bFinalizationCancelled) throw FCollisionBuildCancelled{};
			if (!Geometry)
			{
				Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
				Finish(Result);
				return {};
			}
			Control.Check();
			Result.Status = ECollisionGeometryBuildStatus::Success;
			Result.RetainedVertices = static_cast<uint32>(HullVertices.size());
			Result.SourceTriangles = static_cast<uint32>(FinalFaces.size());
			Result.RetainedTriangles = Result.SourceTriangles;
			Result.RetainedBytes = Geometry.GetRetainedBytes();
			Result.EstimatedPeakBytes = Result.RetainedBytes
				+ Vertices.capacity() * sizeof(FVector3)
				+ Faces.capacity() * sizeof(FFace)
				+ HullVertices.capacity() * sizeof(FVector3)
				+ HullIndices.capacity() * sizeof(uint32);
			Finish(Result);
			return Geometry;
		}
		catch (const FCollisionBuildCancelled&)
		{
			Result.Status = ECollisionGeometryBuildStatus::Cancelled;
			Finish(Result);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			Result.Status = ECollisionGeometryBuildStatus::AllocationFailed;
			Finish(Result);
			return {};
		}
	}

	auto FCollisionGeometryRef::MakeTriangleMesh(
		std::span<const FVector3> Vertices,
		std::span<const uint32> Indices,
		std::span<const uint32> SourceOrdinals,
		const std::function<bool()>& ShouldCancel) -> FCollisionGeometryRef
	try
	{
		FCollisionBuildControl Control{ShouldCancel};
		Control.Check();
		if (!ValidateFeatureInput(Vertices, Indices, SourceOrdinals, Control)) return {};
		return FCollisionGeometryRef(MakeFeatureGeometry(
			ECollisionGeometryKind::TriangleMesh, Vertices, Indices, SourceOrdinals, {}, {}, Control));
	}
	catch (const FCollisionBuildCancelled&)
	{
		return {};
	}

	auto FCollisionGeometryRef::MakeCookedTriangleMesh(
		std::span<const FVector3> Vertices,
		std::span<const uint32> Indices,
		std::span<const uint32> SourceOrdinals,
		std::span<const FCollisionGeometryNode> Nodes,
		std::span<const uint32> LeafTriangles,
		const std::function<bool()>& ShouldCancel) -> FCollisionGeometryRef
	try
	{
		FCollisionBuildControl Control{ShouldCancel};
		Control.Check();
		if (!ValidateFeatureInput(Vertices, Indices, SourceOrdinals, Control)
			|| Nodes.empty() || Nodes.size() > Indices.size() / 3 * 2
			|| LeafTriangles.size() != Indices.size() / 3) return {};
		std::vector<bool> VisitedNodes(Nodes.size(), false);
		std::vector<bool> VisitedTriangles(Indices.size() / 3, false);
		std::array<uint32, 128> Stack{};
		uint32 StackCount = 1;
		Stack[0] = 0;
		while (StackCount > 0)
		{
			Control.Tick();
			const uint32 NodeIndex = Stack[--StackCount];
			if (NodeIndex >= Nodes.size() || VisitedNodes[NodeIndex]) return {};
			VisitedNodes[NodeIndex] = true;
			const FCollisionGeometryNode& Node = Nodes[NodeIndex];
			if (!Math::IsFinite(Node.Minimum) || !Math::IsFinite(Node.Maximum)
				|| Node.Minimum.x > Node.Maximum.x || Node.Minimum.y > Node.Maximum.y
				|| Node.Minimum.z > Node.Maximum.z) return {};
			if (Node.IsLeaf())
			{
				const uint32 Count = Node.GetLeafCount();
				if (Count == 0 || Count > 8 || Node.First > LeafTriangles.size()
					|| Count > LeafTriangles.size() - Node.First) return {};
				for (uint32 Offset = 0; Offset < Count; ++Offset)
				{
					Control.Tick();
					const uint32 Triangle = LeafTriangles[Node.First + Offset];
					if (Triangle >= VisitedTriangles.size() || VisitedTriangles[Triangle]) return {};
					VisitedTriangles[Triangle] = true;
					for (uint32 Corner = 0; Corner < 3; ++Corner)
					{
						Control.Tick();
						const FVector3& Vertex = Vertices[Indices[Triangle * 3 + Corner]];
						for (uint32 Axis = 0; Axis < 3; ++Axis)
							if (Vertex[Axis] < Node.Minimum[Axis] || Vertex[Axis] > Node.Maximum[Axis]) return {};
					}
				}
				continue;
			}
			if (Node.First >= Nodes.size() || Node.CountOrSecond >= Nodes.size()
				|| StackCount + 2 > Stack.size()) return {};
			Stack[StackCount++] = Node.CountOrSecond;
			Stack[StackCount++] = Node.First;
		}
		if (!std::ranges::all_of(VisitedNodes, [&Control](bool Value) { Control.Tick(); return Value; })
			|| !std::ranges::all_of(VisitedTriangles, [&Control](bool Value) { Control.Tick(); return Value; })) return {};
		return FCollisionGeometryRef(MakeFeatureGeometry(ECollisionGeometryKind::TriangleMesh,
			Vertices, Indices, SourceOrdinals, Nodes, LeafTriangles, Control));
	}
	catch (const FCollisionBuildCancelled&)
	{
		return {};
	}

	auto FCollisionGeometryRef::BuildTriangleMesh(
		std::span<const FVector3> Vertices,
		std::span<const uint32> Indices,
		FCollisionGeometryBuildDiagnostics* Diagnostics,
		const std::function<bool()>& ShouldCancel) -> FCollisionGeometryRef
	{
		FCollisionGeometryBuildDiagnostics Result;
		Result.SourceVertices = Vertices.size() <= std::numeric_limits<uint32>::max()
			? static_cast<uint32>(Vertices.size()) : std::numeric_limits<uint32>::max();
		Result.SourceTriangles = Indices.size() / 3 <= std::numeric_limits<uint32>::max()
			? static_cast<uint32>(Indices.size() / 3) : std::numeric_limits<uint32>::max();
		auto Finish = [&](FCollisionGeometryBuildDiagnostics Value) {
			if (Diagnostics) *Diagnostics = Value;
		};
		if (Vertices.empty() || Indices.empty() || Indices.size() % 3 != 0)
		{
			Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
			Finish(Result);
			return {};
		}
		if (Indices.size() / 3 > MaximumCollisionTriangles)
		{
			Result.Status = ECollisionGeometryBuildStatus::LimitExceeded;
			Finish(Result);
			return {};
		}
		try
		{
			FCollisionBuildControl Control{ShouldCancel};
			Control.Check();
			for (const FVector3& Vertex : Vertices)
			{
				Control.Tick();
				if (!Math::IsFinite(Vertex))
				{
					Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
					Finish(Result);
					return {};
				}
			}
			std::vector<uint32> RetainedIndices;
			std::vector<uint32> RetainedOrdinals;
			RetainedIndices.reserve(Indices.size());
			RetainedOrdinals.reserve(Indices.size() / 3);
			std::set<std::array<uint32, 3>> Membership;
			for (uint32 Triangle = 0; Triangle < Indices.size() / 3; ++Triangle)
			{
				Control.Tick();
				const uint32 A = Indices[Triangle * 3];
				const uint32 B = Indices[Triangle * 3 + 1];
				const uint32 C = Indices[Triangle * 3 + 2];
				if (A >= Vertices.size() || B >= Vertices.size() || C >= Vertices.size())
				{
					Result.Status = ECollisionGeometryBuildStatus::InvalidInput;
					Finish(Result);
					return {};
				}
				std::array<uint32, 3> Key{A, B, C};
				std::ranges::sort(Key);
				if (A == B || B == C || C == A
					|| Math::LengthSquared(Math::Cross(Vertices[B] - Vertices[A], Vertices[C] - Vertices[A]))
						<= FeatureTolerance * FeatureTolerance
					|| !Membership.insert(Key).second)
				{
					++Result.RemovedTriangles;
					continue;
				}
				RetainedIndices.insert(RetainedIndices.end(), {A, B, C});
				RetainedOrdinals.push_back(Triangle);
			}
			if (RetainedOrdinals.empty())
			{
				Result.Status = ECollisionGeometryBuildStatus::EmptyAfterCleanup;
				Finish(Result);
				return {};
			}
			std::vector<FCollisionGeometryNode> Nodes;
			std::vector<uint32> LeafTriangles;
			if (!BuildDeterministicBvh(
				Vertices, RetainedIndices, RetainedOrdinals, Nodes, LeafTriangles, Result.MaximumDepth, Control))
			{
				Result.Status = ECollisionGeometryBuildStatus::DepthExceeded;
				Finish(Result);
				return {};
			}
			FCollisionGeometryRef Geometry(MakeFeatureGeometry(ECollisionGeometryKind::TriangleMesh,
				Vertices, RetainedIndices, RetainedOrdinals, Nodes, LeafTriangles, Control));
			Control.Check();
			Result.Status = ECollisionGeometryBuildStatus::Success;
			Result.RetainedVertices = static_cast<uint32>(Vertices.size());
			Result.RetainedTriangles = static_cast<uint32>(RetainedOrdinals.size());
			Result.NodeCount = static_cast<uint32>(Nodes.size());
			Result.RetainedBytes = Geometry.GetRetainedBytes();
			Result.EstimatedPeakBytes = Result.RetainedBytes
				+ Vertices.size_bytes() + Indices.size_bytes()
				+ RetainedIndices.capacity() * sizeof(uint32)
				+ RetainedOrdinals.capacity() * sizeof(uint32)
				+ Nodes.capacity() * sizeof(FCollisionGeometryNode)
				+ LeafTriangles.capacity() * sizeof(uint32)
				+ RetainedOrdinals.size() * sizeof(FTriangleBuildRecord);
			Finish(Result);
			return Geometry;
		}
		catch (const FCollisionBuildCancelled&)
		{
			Result.Status = ECollisionGeometryBuildStatus::Cancelled;
			Finish(Result);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			Result.Status = ECollisionGeometryBuildStatus::AllocationFailed;
			Finish(Result);
			return {};
		}
	}


	auto FCollisionGeometryRef::GetKind() const -> ECollisionGeometryKind
	{
		return GetEncodedGeometryKind(Payload.get());
	}

	auto FCollisionGeometryRef::GetIdentity() const -> uint64
	{
		return Payload ? Payload->Identity : 0;
	}

	auto FCollisionGeometryRef::GetChildCount() const -> uint32
	{
		return Payload ? static_cast<uint32>(Payload->Children.size()) : 0;
	}

	auto FCollisionGeometryRef::GetChild(uint32 Index) const -> const FCollisionGeometryChild*
	{
		return Payload && Index < Payload->Children.size() ? &Payload->Children[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetVertexCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->Vertices.size()) : 0;
	}

	auto FCollisionGeometryRef::GetVertex(uint32 Index) const -> const FVector3*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->Vertices.size() ? &Feature->Vertices[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetTriangleCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->Triangles.size()) : 0;
	}

	auto FCollisionGeometryRef::GetTriangle(uint32 Index) const -> const FCollisionGeometryTriangle*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->Triangles.size() ? &Feature->Triangles[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetTriangleVertices(
		uint32 Index, FVector3& OutFirst, FVector3& OutSecond, FVector3& OutThird,
		uint32* OutSourceOrdinal) const -> bool
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		if (!Feature || Index >= Feature->Triangles.size()) return false;
		const FCollisionGeometryTriangle& Triangle = Feature->Triangles[Index];
		OutFirst = Feature->Vertices[Triangle.First];
		OutSecond = Feature->Vertices[Triangle.Second];
		OutThird = Feature->Vertices[Triangle.Third];
		if (OutSourceOrdinal) *OutSourceOrdinal = Triangle.SourceOrdinal;
		return true;
	}

	auto FCollisionGeometryRef::GetNodeCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->Nodes.size()) : 0;
	}

	auto FCollisionGeometryRef::GetNode(uint32 Index) const -> const FCollisionGeometryNode*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->Nodes.size() ? &Feature->Nodes[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetLeafTriangleCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->LeafTriangles.size()) : 0;
	}

	auto FCollisionGeometryRef::GetLeafTriangle(uint32 Index) const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->LeafTriangles.size()
			? Feature->LeafTriangles[Index] : std::numeric_limits<uint32>::max();
	}

	auto FCollisionGeometryRef::GetHullPlaneCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->HullPlanes.size()) : 0;
	}

	auto FCollisionGeometryRef::GetHullPlane(uint32 Index) const -> const FCollisionHullPlane*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->HullPlanes.size() ? &Feature->HullPlanes[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetHullHalfEdgeCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->HullHalfEdges.size()) : 0;
	}

	auto FCollisionGeometryRef::GetHullHalfEdge(uint32 Index) const -> const FCollisionHullHalfEdge*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->HullHalfEdges.size() ? &Feature->HullHalfEdges[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetHullFaceCount() const -> uint32
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature ? static_cast<uint32>(Feature->HullFaces.size()) : 0;
	}

	auto FCollisionGeometryRef::GetHullFace(uint32 Index) const -> const FCollisionHullFace*
	{
		const FFeatureCollisionGeometry* Feature = GetFeaturePayload(Payload.get());
		return Feature && Index < Feature->HullFaces.size() ? &Feature->HullFaces[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetLocalBounds(FVector3& OutMin, FVector3& OutMax) const -> bool
	{
		if (!Payload) return false;
		OutMin = Payload->LocalMin;
		OutMax = Payload->LocalMax;
		return true;
	}

	auto FCollisionGeometryRef::GetRetainedBytes() const -> uint64
	{
		return Payload ? Payload->RetainedBytes : 0;
	}
}

namespace Durin::CollisionGeometry
{
	namespace
	{
		constexpr double ContactTolerance = 1.0e-8;
		constexpr uint32 SearchIterations = 28;

		struct FCapsuleBoxDistance
		{
			double SquaredDistance = 0.0;
			FVector3 SegmentPoint{0.0};
			FVector3 BoxPoint{0.0};
		};

		auto AddCounter(uint64& Counter, uint64 Delta, bool& bOverflowed) -> void
		{
			if (Counter > std::numeric_limits<uint64>::max() - Delta)
			{
				Counter = std::numeric_limits<uint64>::max();
				bOverflowed = true;
				return;
			}
			Counter += Delta;
		}

		auto RecordGeometryWork(
			FCollisionGeometryCounters* Counters,
			uint64 DistanceEvaluations,
			uint64 SearchIterations) -> void
		{
			if (!Counters) return;
			AddCounter(Counters->DistanceEvaluations, DistanceEvaluations, Counters->bOverflowed);
			AddCounter(Counters->SearchIterations, SearchIterations, Counters->bOverflowed);
		}

		auto NormalizedRotation(const FQuat& Rotation) -> FQuat
		{
			FQuat Result;
			return Math::TryNormalize(Rotation, Result) ? Result : FQuatConstants::Identity;
		}

		auto ToBoxSpace(const FVector3& Point, const FTransform& BoxTransform) -> FVector3
		{
			return Math::RotateVector(
				Math::Inverse(NormalizedRotation(BoxTransform.Rotation)),
				Point - BoxTransform.Translation);
		}

		auto ToWorldDirection(const FVector3& Direction, const FTransform& BoxTransform) -> FVector3
		{
			return Math::RotateVector(NormalizedRotation(BoxTransform.Rotation), Direction);
		}

		auto ClosestPointOnBox(const FVector3& Point, const FVector3& Extent) -> FVector3
		{
			return Math::Clamp(Point, -Extent, Extent);
		}

		auto ExactSegmentBoxDistance(
			const FVector3& SegmentStart,
			const FVector3& SegmentEnd,
			const FVector3& Extent,
			FCollisionGeometryCounters* Counters) -> FCapsuleBoxDistance
		{
			auto Evaluate = [&](double Alpha) {
				const FVector3 SegmentPoint = SegmentStart + (SegmentEnd - SegmentStart) * Alpha;
				const FVector3 BoxPoint = ClosestPointOnBox(SegmentPoint, Extent);
				return FCapsuleBoxDistance{
					.SquaredDistance = Math::LengthSquared(SegmentPoint - BoxPoint),
					.SegmentPoint = SegmentPoint,
					.BoxPoint = BoxPoint};
			};
			const FVector3 Direction = SegmentEnd - SegmentStart;
			std::array<double, 8> Breakpoints{};
			uint32 BreakpointCount = 2;
			Breakpoints[0] = 0.0;
			Breakpoints[1] = 1.0;
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Direction[Axis]) <= ContactTolerance) continue;
				for (double Plane : {-Extent[Axis], Extent[Axis]})
				{
					const double Alpha = (Plane - SegmentStart[Axis]) / Direction[Axis];
					if (Alpha > 0.0 && Alpha < 1.0) Breakpoints[BreakpointCount++] = Alpha;
				}
			}
			std::ranges::sort(Breakpoints.begin(), Breakpoints.begin() + BreakpointCount);
			FCapsuleBoxDistance Result = Evaluate(0.0);
			uint64 EvaluationCount = 1;
			for (uint32 Interval = 0; Interval + 1 < BreakpointCount; ++Interval)
			{
				const double Low = Breakpoints[Interval];
				const double High = Breakpoints[Interval + 1];
				const double Middle = (Low + High) * 0.5;
				const FVector3 MiddlePoint = SegmentStart + Direction * Middle;
				double A = 0.0;
				double B = 0.0;
				for (uint32 Axis = 0; Axis < 3; ++Axis)
				{
					double Plane = MiddlePoint[Axis];
					if (MiddlePoint[Axis] < -Extent[Axis]) Plane = -Extent[Axis];
					else if (MiddlePoint[Axis] > Extent[Axis]) Plane = Extent[Axis];
					else continue;
					A += Direction[Axis] * Direction[Axis];
					B += Direction[Axis] * (SegmentStart[Axis] - Plane);
				}
				const double CandidateAlpha = A > 0.0 ? std::clamp(-B / A, Low, High) : Low;
				for (double Alpha : {Low, CandidateAlpha, High})
				{
					const FCapsuleBoxDistance Candidate = Evaluate(Alpha);
					++EvaluationCount;
					if (Candidate.SquaredDistance < Result.SquaredDistance) Result = Candidate;
				}
			}
			RecordGeometryWork(Counters, EvaluationCount, 0);
			return Result;
		}

		auto SegmentBoxDistance(
			const FVector3& SegmentStart,
			const FVector3& SegmentEnd,
			const FVector3& Extent,
			FCollisionGeometryCounters* Counters) -> FCapsuleBoxDistance
		{
			auto Evaluate = [&](double Alpha) {
				const FVector3 SegmentPoint = SegmentStart + (SegmentEnd - SegmentStart) * Alpha;
				const FVector3 BoxPoint = ClosestPointOnBox(SegmentPoint, Extent);
				return FCapsuleBoxDistance{
					.SquaredDistance = Math::LengthSquared(SegmentPoint - BoxPoint),
					.SegmentPoint = SegmentPoint,
					.BoxPoint = BoxPoint};
			};
			double Low = 0.0;
			double High = 1.0;
			for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
			{
				const double First = (Low * 2.0 + High) / 3.0;
				const double Second = (Low + High * 2.0) / 3.0;
				if (Evaluate(First).SquaredDistance <= Evaluate(Second).SquaredDistance) High = Second;
				else Low = First;
			}
			FCapsuleBoxDistance Result = Evaluate((Low + High) * 0.5);
			const FCapsuleBoxDistance StartResult = Evaluate(0.0);
			const FCapsuleBoxDistance EndResult = Evaluate(1.0);
			if (StartResult.SquaredDistance < Result.SquaredDistance) Result = StartResult;
			if (EndResult.SquaredDistance < Result.SquaredDistance) Result = EndResult;
			RecordGeometryWork(Counters, SearchIterations * 2u + 3u, SearchIterations);
			return Result;
		}

		auto GetCapsuleSegment(
			const FCollisionShape& Capsule,
			const FTransform& CapsuleTransform,
			FVector3& OutStart,
			FVector3& OutEnd,
			double& OutRadius) -> bool
		{
			if (Capsule.GetType() != ECollisionShapeType::Capsule
				|| !Capsule.IsValid() || !IsValidPhysicsTransform(CapsuleTransform)) return false;
			const double RadialScale = std::max(CapsuleTransform.Scale3D.x, CapsuleTransform.Scale3D.y);
			OutRadius = Capsule.GetCapsuleRadius() * RadialScale;
			const double HalfHeight = std::max(
				OutRadius, Capsule.GetCapsuleHalfHeight() * CapsuleTransform.Scale3D.z);
			const FVector3 Axis = Math::RotateVector(
				NormalizedRotation(CapsuleTransform.Rotation), FVectorConstants::Up);
			const FVector3 Offset = Axis * (HalfHeight - OutRadius);
			OutStart = CapsuleTransform.Translation - Offset;
			OutEnd = CapsuleTransform.Translation + Offset;
			return Math::IsFinite(OutStart) && Math::IsFinite(OutEnd) && std::isfinite(OutRadius);
		}

		auto MakeCapsuleContact(
			const FCapsuleBoxDistance& Distance,
			double Radius,
			const FTransform& BoxTransform,
			const FVector3& FallbackDirection,
			FPhysicsQueryHit& OutHit) -> void
		{
			FVector3 LocalNormal;
			if (!Math::TryNormalize(Distance.SegmentPoint - Distance.BoxPoint, LocalNormal))
			{
				const FVector3 Direction = Math::NormalizeOr(FallbackDirection, FVectorConstants::Up);
				LocalNormal = Math::RotateVector(
					Math::Inverse(NormalizedRotation(BoxTransform.Rotation)), -Direction);
				const FVector3 AbsNormal = Math::Abs(LocalNormal);
				const uint32 Axis = AbsNormal.y > AbsNormal.x
					? (AbsNormal.z > AbsNormal.y ? 2u : 1u)
					: (AbsNormal.z > AbsNormal.x ? 2u : 0u);
				LocalNormal = FVector3(0.0);
				LocalNormal[Axis] = Direction[Axis] >= 0.0 ? -1.0 : 1.0;
			}
			OutHit.ImpactNormal = Math::NormalizeOr(ToWorldDirection(LocalNormal, BoxTransform), FVectorConstants::Up);
			OutHit.ImpactPoint = BoxTransform.Translation + ToWorldDirection(Distance.BoxPoint, BoxTransform);
			OutHit.PenetrationDepth = std::max(0.0, Radius - std::sqrt(std::max(0.0, Distance.SquaredDistance)));
		}
	}

	auto RaycastBox(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters*) -> bool
	{
		OutHit = {};
		if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)
			|| !Math::IsFinite(Start) || !Math::IsFinite(End)) return false;
		const FVector3 LocalStart = ToBoxSpace(Start, BoxTransform);
		const FVector3 LocalEnd = ToBoxSpace(End, BoxTransform);
		const FVector3 Delta = LocalEnd - LocalStart;
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		double Enter = 0.0;
		double Exit = 1.0;
		FVector3 EnterNormal(0.0);
		const bool bInside = std::abs(LocalStart.x) <= Extent.x
			&& std::abs(LocalStart.y) <= Extent.y
			&& std::abs(LocalStart.z) <= Extent.z;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			if (std::abs(Delta[Axis]) <= ContactTolerance)
			{
				if (LocalStart[Axis] < -Extent[Axis] || LocalStart[Axis] > Extent[Axis]) return false;
				continue;
			}
			double Near = (-Extent[Axis] - LocalStart[Axis]) / Delta[Axis];
			double Far = (Extent[Axis] - LocalStart[Axis]) / Delta[Axis];
			double Sign = -1.0;
			if (Near > Far)
			{
				std::swap(Near, Far);
				Sign = 1.0;
			}
			if (Near > Enter)
			{
				Enter = Near;
				EnterNormal = FVector3(0.0);
				EnterNormal[Axis] = Sign;
			}
			Exit = std::min(Exit, Far);
			if (Enter > Exit) return false;
		}
		if (Exit < 0.0 || Enter > 1.0) return false;
		OutHit.Time = bInside ? 0.0 : std::clamp(Enter, 0.0, 1.0);
		OutHit.Location = Start + (End - Start) * OutHit.Time;
		OutHit.ImpactPoint = OutHit.Location;
		OutHit.bStartPenetrating = bInside;
		if (bInside)
		{
			double BestDepth = std::numeric_limits<double>::max();
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Depth = Extent[Axis] - std::abs(LocalStart[Axis]);
				if (Depth < BestDepth)
				{
					BestDepth = Depth;
					EnterNormal = FVector3(0.0);
					EnterNormal[Axis] = LocalStart[Axis] >= 0.0 ? 1.0 : -1.0;
				}
			}
			OutHit.PenetrationDepth = BestDepth;
		}
		OutHit.ImpactNormal = Math::NormalizeOr(ToWorldDirection(EnterNormal, BoxTransform), FVectorConstants::Up);
		return true;
	}

	auto OverlapCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> bool
	{
		OutHit = {};
		if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)) return false;
		FVector3 SegmentStart;
		FVector3 SegmentEnd;
		double Radius = 0.0;
		if (!GetCapsuleSegment(Capsule, CapsuleTransform, SegmentStart, SegmentEnd, Radius)) return false;
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		const FCapsuleBoxDistance Distance = SegmentBoxDistance(
			ToBoxSpace(SegmentStart, BoxTransform), ToBoxSpace(SegmentEnd, BoxTransform), Extent, Counters);
		if (Distance.SquaredDistance >= Radius * Radius - ContactTolerance) return false;
		OutHit.Time = 0.0;
		OutHit.Location = CapsuleTransform.Translation;
		OutHit.bStartPenetrating = true;
		MakeCapsuleContact(Distance, Radius, BoxTransform, FVectorConstants::Up, OutHit);
		return true;
	}

	auto SweepCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FVector3& Delta,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> bool
	{
		OutHit = {};
		if (!Math::IsFinite(Delta)) return false;
		if (OverlapCapsuleBox(Capsule, CapsuleTransform, Box, BoxTransform, OutHit, Counters)) return true;
		FVector3 SegmentStart;
		FVector3 SegmentEnd;
		double Radius = 0.0;
		if (!GetCapsuleSegment(Capsule, CapsuleTransform, SegmentStart, SegmentEnd, Radius)
			|| Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)) return false;
		const FVector3 LocalStart = ToBoxSpace(SegmentStart, BoxTransform);
		const FVector3 LocalEnd = ToBoxSpace(SegmentEnd, BoxTransform);
		const FVector3 LocalDelta = Math::RotateVector(
			Math::Inverse(NormalizedRotation(BoxTransform.Rotation)), Delta);
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		auto Evaluate = [&](double Time) {
			return SegmentBoxDistance(
				LocalStart + LocalDelta * Time, LocalEnd + LocalDelta * Time, Extent, Counters);
		};
		double Low = 0.0;
		double High = 1.0;
		for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
		{
			const double First = (Low * 2.0 + High) / 3.0;
			const double Second = (Low + High * 2.0) / 3.0;
			if (Evaluate(First).SquaredDistance <= Evaluate(Second).SquaredDistance) High = Second;
			else Low = First;
		}
		RecordGeometryWork(Counters, 0, SearchIterations);
		const double MinimumTime = (Low + High) * 0.5;
		if (Evaluate(MinimumTime).SquaredDistance > Radius * Radius + ContactTolerance) return false;
		Low = 0.0;
		High = MinimumTime;
		for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
		{
			const double Middle = (Low + High) * 0.5;
			if (Evaluate(Middle).SquaredDistance <= Radius * Radius + ContactTolerance) High = Middle;
			else Low = Middle;
		}
		RecordGeometryWork(Counters, 0, SearchIterations);
		OutHit.Time = std::clamp(High, 0.0, 1.0);
		OutHit.Location = CapsuleTransform.Translation + Delta * OutHit.Time;
		MakeCapsuleContact(Evaluate(OutHit.Time), Radius, BoxTransform, Delta, OutHit);
		if (Math::Dot(Delta, OutHit.ImpactNormal) >= -ContactTolerance)
		{
			OutHit = {};
			return false;
		}
		return true;
	}

	namespace
	{
		struct FLeafDistance
		{
			double Separation = 0.0;
			FVector3 Normal{0.0};
			FVector3 TargetPoint{0.0};
			FVector3 QueryPoint{0.0};
		};

		auto GetSphere(
			const FCollisionShape& Shape, const FTransform& Transform,
			FVector3& OutCenter, double& OutRadius) -> bool
		{
			if (Shape.GetType() != ECollisionShapeType::Sphere || !Shape.IsValid()
				|| !IsValidPhysicsTransform(Transform)) return false;
			OutCenter = Transform.Translation;
			OutRadius = Shape.GetSphereRadius() * std::max({
				Transform.Scale3D.x, Transform.Scale3D.y, Transform.Scale3D.z});
			return std::isfinite(OutRadius);
		}

		auto ClosestPointOnSegment(const FVector3& Point, const FVector3& Start, const FVector3& End)
			-> FVector3
		{
			const FVector3 Delta = End - Start;
			const double LengthSquared = Math::LengthSquared(Delta);
			const double Alpha = LengthSquared > 0.0
				? std::clamp(Math::Dot(Point - Start, Delta) / LengthSquared, 0.0, 1.0) : 0.0;
			return Start + Delta * Alpha;
		}

		auto ClosestSegmentPoints(
			const FVector3& FirstStart, const FVector3& FirstEnd,
			const FVector3& SecondStart, const FVector3& SecondEnd,
			FVector3& OutFirst, FVector3& OutSecond) -> void
		{
			const FVector3 D1 = FirstEnd - FirstStart;
			const FVector3 D2 = SecondEnd - SecondStart;
			const FVector3 R = FirstStart - SecondStart;
			const double A = Math::Dot(D1, D1);
			const double E = Math::Dot(D2, D2);
			const double F = Math::Dot(D2, R);
			double S = 0.0;
			double T = 0.0;
			if (A <= ContactTolerance && E <= ContactTolerance)
			{
				OutFirst = FirstStart;
				OutSecond = SecondStart;
				return;
			}
			if (A <= ContactTolerance) T = std::clamp(F / E, 0.0, 1.0);
			else
			{
				const double C = Math::Dot(D1, R);
				if (E <= ContactTolerance) S = std::clamp(-C / A, 0.0, 1.0);
				else
				{
					const double B = Math::Dot(D1, D2);
					const double Denominator = A * E - B * B;
					if (Denominator != 0.0) S = std::clamp((B * F - C * E) / Denominator, 0.0, 1.0);
					T = (B * S + F) / E;
					if (T < 0.0)
					{
						T = 0.0;
						S = std::clamp(-C / A, 0.0, 1.0);
					}
					else if (T > 1.0)
					{
						T = 1.0;
						S = std::clamp((B - C) / A, 0.0, 1.0);
					}
				}
			}
			OutFirst = FirstStart + D1 * S;
			OutSecond = SecondStart + D2 * T;
		}

		auto SupportPoint(
			const FCollisionShape& Shape, const FTransform& Transform, const FVector3& Direction,
			FVector3& OutPoint, FCollisionGeometryCounters* Counters) -> bool
		{
			if (Counters) AddCounter(Counters->SupportEvaluations, 1, Counters->bOverflowed);
			const FVector3 Normal = Math::NormalizeOr(Direction, FVectorConstants::Forward);
			if (Shape.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 Center;
				double Radius = 0.0;
				if (!GetSphere(Shape, Transform, Center, Radius)) return false;
				OutPoint = Center + Normal * Radius;
				return true;
			}
			if (Shape.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 Start;
				FVector3 End;
				double Radius = 0.0;
				if (!GetCapsuleSegment(Shape, Transform, Start, End, Radius)) return false;
				OutPoint = (Math::Dot(Start, Normal) >= Math::Dot(End, Normal) ? Start : End)
					+ Normal * Radius;
				return true;
			}
			if (Shape.GetType() != ECollisionShapeType::Box || !Shape.IsValid()
				|| !IsValidPhysicsTransform(Transform)) return false;
			FQuat Rotation;
			if (!Math::TryNormalize(Transform.Rotation, Rotation)) return false;
			const std::array<FVector3, 3> Axes{
				Math::RotateVector(Rotation, FVectorConstants::Forward),
				Math::RotateVector(Rotation, FVectorConstants::Right),
				Math::RotateVector(Rotation, FVectorConstants::Up)};
			const FVector3 Extent = Shape.GetBoxHalfExtent() * Transform.Scale3D;
			OutPoint = Transform.Translation;
			for (uint32 Index = 0; Index < 3; ++Index)
				OutPoint += Axes[Index] * Extent[Index] * (Math::Dot(Normal, Axes[Index]) >= 0.0 ? 1.0 : -1.0);
			return true;
		}

		auto PointBoxDistance(
			const FVector3& Point, const FCollisionShape& Box, const FTransform& BoxTransform,
			FLeafDistance& OutDistance) -> bool
		{
			if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
				|| !IsValidPhysicsTransform(BoxTransform)) return false;
			const FVector3 LocalPoint = ToBoxSpace(Point, BoxTransform);
			const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
			FVector3 LocalClosest = ClosestPointOnBox(LocalPoint, Extent);
			const FVector3 Delta = LocalPoint - LocalClosest;
			const double Distance = Math::Length(Delta);
			FVector3 LocalNormal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
			if (Distance <= ContactTolerance)
			{
				double BestDepth = std::numeric_limits<double>::max();
				for (uint32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Depth = Extent[Axis] - std::abs(LocalPoint[Axis]);
					if (Depth < BestDepth)
					{
						BestDepth = Depth;
						LocalNormal = FVector3(0.0);
						LocalNormal[Axis] = LocalPoint[Axis] >= 0.0 ? 1.0 : -1.0;
						LocalClosest[Axis] = LocalNormal[Axis] * Extent[Axis];
					}
				}
				OutDistance.Separation = -BestDepth;
			}
			else OutDistance.Separation = Distance;
			OutDistance.Normal = Math::NormalizeOr(ToWorldDirection(LocalNormal, BoxTransform), FVectorConstants::Forward);
			OutDistance.TargetPoint = BoxTransform.Translation + ToWorldDirection(LocalClosest, BoxTransform);
			OutDistance.QueryPoint = Point;
			return true;
		}

		auto PrimitiveDistance(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FLeafDistance& OutDistance, FCollisionGeometryCounters* Counters) -> bool
		{
			RecordGeometryWork(Counters, 1, 0);
			if (!Query.IsValid() || !Target.IsValid() || !IsValidPhysicsTransform(QueryTransform)
				|| !IsValidPhysicsTransform(TargetTransform)) return false;
			if (Query.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 QueryCenter;
				double QueryRadius = 0.0;
				if (!GetSphere(Query, QueryTransform, QueryCenter, QueryRadius)) return false;
				if (Target.GetType() == ECollisionShapeType::Sphere)
				{
					FVector3 TargetCenter;
					double TargetRadius = 0.0;
					if (!GetSphere(Target, TargetTransform, TargetCenter, TargetRadius)) return false;
					const FVector3 Delta = QueryCenter - TargetCenter;
					const double CenterDistance = Math::Length(Delta);
					OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
					OutDistance.Separation = CenterDistance - QueryRadius - TargetRadius;
					OutDistance.TargetPoint = TargetCenter + OutDistance.Normal * TargetRadius;
					OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
					return true;
				}
				if (Target.GetType() == ECollisionShapeType::Box)
				{
					if (!PointBoxDistance(QueryCenter, Target, TargetTransform, OutDistance)) return false;
					OutDistance.Separation -= QueryRadius;
					OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
					return true;
				}
				FVector3 TargetStart;
				FVector3 TargetEnd;
				double TargetRadius = 0.0;
				if (!GetCapsuleSegment(Target, TargetTransform, TargetStart, TargetEnd, TargetRadius)) return false;
				const FVector3 TargetAxisPoint = ClosestPointOnSegment(QueryCenter, TargetStart, TargetEnd);
				const FVector3 Delta = QueryCenter - TargetAxisPoint;
				const double CenterDistance = Math::Length(Delta);
				OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
				OutDistance.Separation = CenterDistance - QueryRadius - TargetRadius;
				OutDistance.TargetPoint = TargetAxisPoint + OutDistance.Normal * TargetRadius;
				OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Query.GetType() == ECollisionShapeType::Capsule
				&& Target.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 QueryStart;
				FVector3 QueryEnd;
				FVector3 TargetStart;
				FVector3 TargetEnd;
				double QueryRadius = 0.0;
				double TargetRadius = 0.0;
				if (!GetCapsuleSegment(Query, QueryTransform, QueryStart, QueryEnd, QueryRadius)
					|| !GetCapsuleSegment(Target, TargetTransform, TargetStart, TargetEnd, TargetRadius)) return false;
				FVector3 QueryPoint;
				FVector3 TargetPoint;
				ClosestSegmentPoints(QueryStart, QueryEnd, TargetStart, TargetEnd, QueryPoint, TargetPoint);
				const FVector3 Delta = QueryPoint - TargetPoint;
				const double AxisDistance = Math::Length(Delta);
				OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
				OutDistance.Separation = AxisDistance - QueryRadius - TargetRadius;
				OutDistance.TargetPoint = TargetPoint + OutDistance.Normal * TargetRadius;
				OutDistance.QueryPoint = QueryPoint - OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Query.GetType() == ECollisionShapeType::Capsule
				&& Target.GetType() == ECollisionShapeType::Box)
			{
				FVector3 QueryStart;
				FVector3 QueryEnd;
				double QueryRadius = 0.0;
				if (!GetCapsuleSegment(Query, QueryTransform, QueryStart, QueryEnd, QueryRadius)) return false;
				const FVector3 Extent = Target.GetBoxHalfExtent() * TargetTransform.Scale3D;
				const FCapsuleBoxDistance Distance = ExactSegmentBoxDistance(
					ToBoxSpace(QueryStart, TargetTransform), ToBoxSpace(QueryEnd, TargetTransform), Extent, Counters);
				const FVector3 LocalDelta = Distance.SegmentPoint - Distance.BoxPoint;
				OutDistance.Normal = Math::NormalizeOr(
					ToWorldDirection(LocalDelta, TargetTransform), FVectorConstants::Forward);
				OutDistance.Separation = std::sqrt(std::max(0.0, Distance.SquaredDistance)) - QueryRadius;
				OutDistance.TargetPoint = TargetTransform.Translation
					+ ToWorldDirection(Distance.BoxPoint, TargetTransform);
				OutDistance.QueryPoint = TargetTransform.Translation
					+ ToWorldDirection(Distance.SegmentPoint, TargetTransform)
					- OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Target.GetType() == ECollisionShapeType::Sphere
				|| (Query.GetType() == ECollisionShapeType::Box
					&& Target.GetType() == ECollisionShapeType::Capsule))
			{
				FLeafDistance Reverse;
				if (!PrimitiveDistance(Target, TargetTransform, Query, QueryTransform, Reverse, Counters)) return false;
				OutDistance.Separation = Reverse.Separation;
				OutDistance.Normal = -Reverse.Normal;
				OutDistance.TargetPoint = Reverse.QueryPoint;
				OutDistance.QueryPoint = Reverse.TargetPoint;
				return true;
			}
			// Box/Box uses all face and edge-cross separating axes. The largest gap is a conservative cast step.
			if (Query.GetType() == ECollisionShapeType::Box && Target.GetType() == ECollisionShapeType::Box)
			{
				FQuat QueryRotation;
				FQuat TargetRotation;
				if (!Math::TryNormalize(QueryTransform.Rotation, QueryRotation)
					|| !Math::TryNormalize(TargetTransform.Rotation, TargetRotation)) return false;
				const std::array<FVector3, 3> QueryAxes{
					Math::RotateVector(QueryRotation, FVectorConstants::Forward),
					Math::RotateVector(QueryRotation, FVectorConstants::Right),
					Math::RotateVector(QueryRotation, FVectorConstants::Up)};
				const std::array<FVector3, 3> TargetAxes{
					Math::RotateVector(TargetRotation, FVectorConstants::Forward),
					Math::RotateVector(TargetRotation, FVectorConstants::Right),
					Math::RotateVector(TargetRotation, FVectorConstants::Up)};
				const FVector3 QueryExtent = Query.GetBoxHalfExtent() * QueryTransform.Scale3D;
				const FVector3 TargetExtent = Target.GetBoxHalfExtent() * TargetTransform.Scale3D;
				const FVector3 Centers = QueryTransform.Translation - TargetTransform.Translation;
				double MaximumGap = -std::numeric_limits<double>::max();
				FVector3 BestAxis = FVectorConstants::Forward;
				auto TestAxis = [&](const FVector3& Candidate) {
					FVector3 Axis;
					if (!Math::TryNormalize(Candidate, Axis)) return;
					double QueryRadius = 0.0;
					double TargetRadius = 0.0;
					for (uint32 Index = 0; Index < 3; ++Index)
					{
						QueryRadius += std::abs(Math::Dot(Axis, QueryAxes[Index])) * QueryExtent[Index];
						TargetRadius += std::abs(Math::Dot(Axis, TargetAxes[Index])) * TargetExtent[Index];
					}
					const double SignedCenter = Math::Dot(Centers, Axis);
					const double Gap = std::abs(SignedCenter) - QueryRadius - TargetRadius;
					if (Gap > MaximumGap)
					{
						MaximumGap = Gap;
						BestAxis = SignedCenter >= 0.0 ? Axis : -Axis;
					}
				};
				for (const FVector3& Axis : QueryAxes) TestAxis(Axis);
				for (const FVector3& Axis : TargetAxes) TestAxis(Axis);
				for (const FVector3& QueryAxis : QueryAxes)
					for (const FVector3& TargetAxis : TargetAxes) TestAxis(Math::Cross(QueryAxis, TargetAxis));
				OutDistance.Separation = MaximumGap;
				OutDistance.Normal = BestAxis;
				return SupportPoint(Target, TargetTransform, BestAxis, OutDistance.TargetPoint, Counters)
					&& SupportPoint(Query, QueryTransform, -BestAxis, OutDistance.QueryPoint, Counters);
			}
			return false;
		}

		auto TransformFeatureVertex(
			const FCollisionGeometryRef& Geometry, uint32 Index, const FTransform& Transform,
			FVector3& OutVertex) -> bool
		{
			const FVector3* Vertex = Geometry.GetVertex(Index);
			if (!Vertex || !IsValidPhysicsTransform(Transform)) return false;
			OutVertex = Transform.Translation + Math::RotateVector(
				NormalizedRotation(Transform.Rotation), Transform.Scale3D * *Vertex);
			return Math::IsFinite(OutVertex);
		}

		auto GetWorldTriangle(
			const FCollisionGeometryRef& Geometry, uint32 Index, const FTransform& Transform,
			FVector3& OutA, FVector3& OutB, FVector3& OutC, uint32& OutOrdinal) -> bool
		{
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(Index);
			if (!Triangle) return false;
			OutOrdinal = Triangle->SourceOrdinal;
			return TransformFeatureVertex(Geometry, Triangle->First, Transform, OutA)
				&& TransformFeatureVertex(Geometry, Triangle->Second, Transform, OutB)
				&& TransformFeatureVertex(Geometry, Triangle->Third, Transform, OutC);
		}

		auto ClosestPointOnTriangle(
			const FVector3& Point, const FVector3& A, const FVector3& B, const FVector3& C) -> FVector3
		{
			const FVector3 AB = B - A;
			const FVector3 AC = C - A;
			const FVector3 AP = Point - A;
			const double D1 = Math::Dot(AB, AP);
			const double D2 = Math::Dot(AC, AP);
			if (D1 <= 0.0 && D2 <= 0.0) return A;
			const FVector3 BP = Point - B;
			const double D3 = Math::Dot(AB, BP);
			const double D4 = Math::Dot(AC, BP);
			if (D3 >= 0.0 && D4 <= D3) return B;
			const double VC = D1 * D4 - D3 * D2;
			if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0) return A + AB * (D1 / (D1 - D3));
			const FVector3 CP = Point - C;
			const double D5 = Math::Dot(AB, CP);
			const double D6 = Math::Dot(AC, CP);
			if (D6 >= 0.0 && D5 <= D6) return C;
			const double VB = D5 * D2 - D1 * D6;
			if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0) return A + AC * (D2 / (D2 - D6));
			const double VA = D3 * D6 - D5 * D4;
			if (VA <= 0.0 && D4 - D3 >= 0.0 && D5 - D6 >= 0.0)
				return B + (C - B) * ((D4 - D3) / ((D4 - D3) + (D5 - D6)));
			const double Denominator = 1.0 / (VA + VB + VC);
			return A + AB * (VB * Denominator) + AC * (VC * Denominator);
		}

		auto SegmentTriangleDistance(
			const FVector3& Start, const FVector3& End,
			const FVector3& A, const FVector3& B, const FVector3& C,
			FVector3& OutSegment, FVector3& OutTriangle) -> double
		{
			const FVector3 Direction = End - Start;
			const FVector3 Normal = Math::Cross(B - A, C - A);
			const double Denominator = Math::Dot(Normal, Direction);
			if (std::abs(Denominator) > ContactTolerance)
			{
				const double Time = Math::Dot(Normal, A - Start) / Denominator;
				if (Time >= 0.0 && Time <= 1.0)
				{
					const FVector3 Point = Start + Direction * Time;
					if (Math::LengthSquared(Point - ClosestPointOnTriangle(Point, A, B, C))
						<= ContactTolerance * ContactTolerance)
					{
						OutSegment = Point;
						OutTriangle = Point;
						return 0.0;
					}
				}
			}
			OutSegment = Start;
			OutTriangle = ClosestPointOnTriangle(Start, A, B, C);
			double Best = Math::LengthSquared(OutSegment - OutTriangle);
			auto Test = [&](const FVector3& SegmentPoint, const FVector3& TrianglePoint) {
				const double Candidate = Math::LengthSquared(SegmentPoint - TrianglePoint);
				if (Candidate < Best)
				{
					Best = Candidate;
					OutSegment = SegmentPoint;
					OutTriangle = TrianglePoint;
				}
			};
			Test(End, ClosestPointOnTriangle(End, A, B, C));
			for (const auto& Edge : {std::pair{A, B}, std::pair{B, C}, std::pair{C, A}})
			{
				FVector3 SegmentPoint;
				FVector3 TrianglePoint;
				ClosestSegmentPoints(Start, End, Edge.first, Edge.second, SegmentPoint, TrianglePoint);
				Test(SegmentPoint, TrianglePoint);
			}
			return Best;
		}

		auto TriangleDistance(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FVector3& A, const FVector3& B, const FVector3& C,
			FLeafDistance& OutDistance, FCollisionGeometryCounters* Counters) -> bool
		{
			RecordGeometryWork(Counters, 1, 0);
			if (Counters) AddCounter(Counters->FeatureTests, 1, Counters->bOverflowed);
			if (Query.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 Center;
				double Radius = 0.0;
				if (!GetSphere(Query, QueryTransform, Center, Radius)) return false;
				OutDistance.TargetPoint = ClosestPointOnTriangle(Center, A, B, C);
				const FVector3 Delta = Center - OutDistance.TargetPoint;
				const double Distance = Math::Length(Delta);
				const FVector3 TriangleNormal = Math::NormalizeOr(Math::Cross(B - A, C - A), FVectorConstants::Up);
				OutDistance.Normal = Math::NormalizeOr(Delta,
					Math::Dot(Center - A, TriangleNormal) >= 0.0 ? TriangleNormal : -TriangleNormal);
				OutDistance.Separation = Distance - Radius;
				OutDistance.QueryPoint = Center - OutDistance.Normal * Radius;
				return true;
			}
			if (Query.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 Start;
				FVector3 End;
				double Radius = 0.0;
				if (!GetCapsuleSegment(Query, QueryTransform, Start, End, Radius)) return false;
				FVector3 SegmentPoint;
				const double SquaredDistance = SegmentTriangleDistance(
					Start, End, A, B, C, SegmentPoint, OutDistance.TargetPoint);
				const FVector3 Delta = SegmentPoint - OutDistance.TargetPoint;
				const FVector3 TriangleNormal = Math::NormalizeOr(Math::Cross(B - A, C - A), FVectorConstants::Up);
				OutDistance.Normal = Math::NormalizeOr(Delta,
					Math::Dot(QueryTransform.Translation - A, TriangleNormal) >= 0.0 ? TriangleNormal : -TriangleNormal);
				OutDistance.Separation = std::sqrt(std::max(0.0, SquaredDistance)) - Radius;
				OutDistance.QueryPoint = SegmentPoint - OutDistance.Normal * Radius;
				return true;
			}
			if (Query.GetType() != ECollisionShapeType::Box) return false;
			FQuat Rotation;
			if (!Math::TryNormalize(QueryTransform.Rotation, Rotation)) return false;
			const FQuat InverseRotation = Math::Inverse(Rotation);
			auto ToLocal = [&](const FVector3& Point) {
				return Math::RotateVector(InverseRotation, Point - QueryTransform.Translation);
			};
			const std::array<FVector3, 3> LocalVertices{ToLocal(A), ToLocal(B), ToLocal(C)};
			const FVector3 Extent = Query.GetBoxHalfExtent() * QueryTransform.Scale3D;
			double MaximumGap = -std::numeric_limits<double>::max();
			FVector3 BestLocalAxis = FVectorConstants::Forward;
			auto TestAxis = [&](const FVector3& Candidate) {
				FVector3 Axis;
				if (!Math::TryNormalize(Candidate, Axis)) return;
				const double BoxRadius = std::abs(Axis.x) * Extent.x
					+ std::abs(Axis.y) * Extent.y + std::abs(Axis.z) * Extent.z;
				double Minimum = Math::Dot(Axis, LocalVertices[0]);
				double Maximum = Minimum;
				for (uint32 Index = 1; Index < 3; ++Index)
				{
					const double Projection = Math::Dot(Axis, LocalVertices[Index]);
					Minimum = std::min(Minimum, Projection);
					Maximum = std::max(Maximum, Projection);
				}
				const double Gap = std::max(Minimum - BoxRadius, -BoxRadius - Maximum);
				if (Gap > MaximumGap)
				{
					MaximumGap = Gap;
					const double TriangleCenter = (Minimum + Maximum) * 0.5;
					BestLocalAxis = TriangleCenter <= 0.0 ? Axis : -Axis;
				}
			};
			const std::array<FVector3, 3> BoxAxes{
				FVectorConstants::Forward, FVectorConstants::Right, FVectorConstants::Up};
			for (const FVector3& Axis : BoxAxes) TestAxis(Axis);
			const std::array<FVector3, 3> Edges{
				LocalVertices[1] - LocalVertices[0], LocalVertices[2] - LocalVertices[1],
				LocalVertices[0] - LocalVertices[2]};
			TestAxis(Math::Cross(Edges[0], Edges[1]));
			for (const FVector3& Edge : Edges)
				for (const FVector3& Axis : BoxAxes) TestAxis(Math::Cross(Edge, Axis));
			OutDistance.Separation = MaximumGap;
			OutDistance.Normal = Math::RotateVector(Rotation, BestLocalAxis);
			if (!SupportPoint(Query, QueryTransform, -OutDistance.Normal, OutDistance.QueryPoint, Counters)) return false;
			OutDistance.TargetPoint = ClosestPointOnTriangle(OutDistance.QueryPoint, A, B, C);
			return true;
		}


		auto FeatureDistance(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FLeafDistance& OutDistance, uint32& OutOrdinal,
			FCollisionGeometryCounters* Counters) -> bool
		{
			bool bFound = false;
			for (uint32 Index = 0; Index < Target.GetTriangleCount(); ++Index)
			{
				FVector3 A;
				FVector3 B;
				FVector3 C;
				uint32 Ordinal = 0;
				if (!GetWorldTriangle(Target, Index, TargetTransform, A, B, C, Ordinal)) return false;
				FLeafDistance Candidate;
				if (!TriangleDistance(Query, QueryTransform, A, B, C, Candidate, Counters)) return false;
				if (!bFound || Candidate.Separation < OutDistance.Separation
					|| (Candidate.Separation == OutDistance.Separation && Ordinal < OutOrdinal))
				{
					OutDistance = Candidate;
					OutOrdinal = Ordinal;
					bFound = true;
				}
			}
			return bFound;
		}

		auto HullDistance(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FLeafDistance& OutDistance, uint32& OutOrdinal,
			FCollisionGeometryCounters* Counters) -> bool
		{
			bool bFound = false;
			for (uint32 Index = 0; Index < Target.GetTriangleCount(); ++Index)
			{
				FVector3 A;
				FVector3 B;
				FVector3 C;
				uint32 Ordinal = 0;
				if (!GetWorldTriangle(Target, Index, TargetTransform, A, B, C, Ordinal)) return false;
				FVector3 Normal;
				if (!Math::TryNormalize(Math::Cross(B - A, C - A), Normal)) return false;
				FVector3 QueryPoint;
				if (!SupportPoint(Query, QueryTransform, -Normal, QueryPoint, Counters)) return false;
				if (Counters) AddCounter(Counters->FeatureTests, 1, Counters->bOverflowed);
				RecordGeometryWork(Counters, 1, 0);
				const double Separation = Math::Dot(QueryPoint - A, Normal);
				if (!bFound || Separation > OutDistance.Separation
					|| (Separation == OutDistance.Separation && Ordinal < OutOrdinal))
				{
					OutDistance.Separation = Separation;
					OutDistance.Normal = Normal;
					OutDistance.QueryPoint = QueryPoint;
					OutDistance.TargetPoint = QueryPoint - Normal * Separation;
					OutOrdinal = Ordinal;
					bFound = true;
				}
			}
			return bFound;
		}

		auto FeatureOverlap(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> bool
		{
			FLeafDistance Distance;
			uint32 Ordinal = 0;
			const bool bValid = Target.GetKind() == ECollisionGeometryKind::ConvexHull
				? HullDistance(Query, QueryTransform, Target, TargetTransform, Distance, Ordinal, Counters)
				: FeatureDistance(Query, QueryTransform, Target, TargetTransform, Distance, Ordinal, Counters);
			if (!bValid || Distance.Separation >= -ContactTolerance) return false;
			OutHit = {};
			OutHit.Time = 0.0;
			OutHit.Location = QueryTransform.Translation;
			OutHit.ImpactPoint = Distance.TargetPoint;
			OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, FVectorConstants::Forward);
			OutHit.PenetrationDepth = -Distance.Separation;
			OutHit.bStartPenetrating = true;
			return true;
		}

		auto SweepTriangleFeature(
			const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
			const FVector3& A, const FVector3& B, const FVector3& C,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			double Time = 0.0;
			for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			{
				RecordGeometryWork(Counters, 0, 1);
				FTransform Moved = QueryTransform;
				Moved.Translation += Delta * Time;
				FLeafDistance Distance;
				if (!TriangleDistance(Query, Moved, A, B, C, Distance, Counters))
					return ECollisionQueryStatus::Unsupported;
				if (Distance.Separation <= ContactTolerance)
				{
					if (Distance.Separation >= -ContactTolerance
						&& Math::Dot(Delta, Distance.Normal) >= -ContactTolerance)
						return ECollisionQueryStatus::Miss;
					OutHit = {};
					OutHit.Time = std::clamp(Time, 0.0, 1.0);
					OutHit.Location = Moved.Translation;
					OutHit.ImpactPoint = Distance.TargetPoint;
					OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, -Delta);
					if (Time == 0.0 && Distance.Separation < -ContactTolerance)
					{
						OutHit.bStartPenetrating = true;
						OutHit.PenetrationDepth = -Distance.Separation;
					}
					return ECollisionQueryStatus::Hit;
				}
				const double ClosingSpeed = -Math::Dot(Delta, Distance.Normal);
				if (ClosingSpeed <= ContactTolerance) return ECollisionQueryStatus::Miss;
				const double Step = Distance.Separation / ClosingSpeed;
				if (!std::isfinite(Step) || Step <= 1.0e-12)
					return ECollisionQueryStatus::NonConverged;
				Time += Step;
				if (Time > 1.0 + ContactTolerance) return ECollisionQueryStatus::Miss;
			}
			return ECollisionQueryStatus::NonConverged;
		}

		auto FeatureSweep(
			const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			if (FeatureOverlap(Query, QueryTransform, Target, TargetTransform, OutHit, Counters))
				return ECollisionQueryStatus::Hit;
			if (Math::LengthSquared(Delta) <= ContactTolerance * ContactTolerance)
				return ECollisionQueryStatus::Miss;
			double Time = 0.0;
			for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			{
				RecordGeometryWork(Counters, 0, 1);
				FTransform Moved = QueryTransform;
				Moved.Translation += Delta * Time;
				FLeafDistance Distance;
				uint32 Ordinal = 0;
				const bool bValid = Target.GetKind() == ECollisionGeometryKind::ConvexHull
					? HullDistance(Query, Moved, Target, TargetTransform, Distance, Ordinal, Counters)
					: FeatureDistance(Query, Moved, Target, TargetTransform, Distance, Ordinal, Counters);
				if (!bValid) return ECollisionQueryStatus::Unsupported;
				if (Distance.Separation <= ContactTolerance)
				{
					if (Math::Dot(Delta, Distance.Normal) >= -ContactTolerance)
						return ECollisionQueryStatus::Miss;
					OutHit = {};
					OutHit.Time = std::clamp(Time, 0.0, 1.0);
					OutHit.Location = Moved.Translation;
					OutHit.ImpactPoint = Distance.TargetPoint;
					OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, -Delta);
					return ECollisionQueryStatus::Hit;
				}
				const double ClosingSpeed = -Math::Dot(Delta, Distance.Normal);
				if (ClosingSpeed <= ContactTolerance) return ECollisionQueryStatus::Miss;
				const double Step = Distance.Separation / ClosingSpeed;
				if (!std::isfinite(Step) || Step <= 1.0e-12) return ECollisionQueryStatus::NonConverged;
				Time += Step;
				if (Time > 1.0 + ContactTolerance) return ECollisionQueryStatus::Miss;
			}
			return ECollisionQueryStatus::NonConverged;
		}

		auto RaycastTriangle(
			const FVector3& Start, const FVector3& End,
			const FVector3& A, const FVector3& B, const FVector3& C,
			FPhysicsQueryHit& OutHit) -> bool
		{
			const FVector3 Delta = End - Start;
			const FVector3 Edge1 = B - A;
			const FVector3 Edge2 = C - A;
			const FVector3 P = Math::Cross(Delta, Edge2);
			const double Determinant = Math::Dot(Edge1, P);
			if (std::abs(Determinant) <= ContactTolerance) return false;
			const double Inverse = 1.0 / Determinant;
			const FVector3 T = Start - A;
			const double U = Math::Dot(T, P) * Inverse;
			if (U < -ContactTolerance || U > 1.0 + ContactTolerance) return false;
			const FVector3 Q = Math::Cross(T, Edge1);
			const double V = Math::Dot(Delta, Q) * Inverse;
			if (V < -ContactTolerance || U + V > 1.0 + ContactTolerance) return false;
			const double Time = Math::Dot(Edge2, Q) * Inverse;
			if (Time < -ContactTolerance || Time > 1.0 + ContactTolerance) return false;
			OutHit = {};
			OutHit.Time = std::clamp(Time, 0.0, 1.0);
			OutHit.Location = Start + Delta * OutHit.Time;
			OutHit.ImpactPoint = OutHit.Location;
			FVector3 Normal = Math::NormalizeOr(Math::Cross(Edge1, Edge2), FVectorConstants::Up);
			if (Math::Dot(Normal, Delta) > 0.0) Normal = -Normal;
			OutHit.ImpactNormal = Normal;
			return true;
		}

		auto BuildShapeWorldBounds(
			const FCollisionShape& Shape, const FTransform& Transform,
			FVector3& OutMinimum, FVector3& OutMaximum) -> bool
		{
			if (!Shape.IsValid() || !IsValidPhysicsTransform(Transform)) return false;
			if (Shape.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 Center;
				double Radius = 0.0;
				if (!GetSphere(Shape, Transform, Center, Radius)) return false;
				OutMinimum = Center - FVector3(Radius);
				OutMaximum = Center + FVector3(Radius);
				return true;
			}
			if (Shape.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 Start;
				FVector3 End;
				double Radius = 0.0;
				if (!GetCapsuleSegment(Shape, Transform, Start, End, Radius)) return false;
				OutMinimum = Math::Min(Start, End) - FVector3(Radius);
				OutMaximum = Math::Max(Start, End) + FVector3(Radius);
				return true;
			}
			FQuat Rotation;
			if (!Math::TryNormalize(Transform.Rotation, Rotation)) return false;
			const FVector3 Extent = Shape.GetBoxHalfExtent() * Transform.Scale3D;
			const FVector3 WorldExtent =
				Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Forward)) * Extent.x
				+ Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Right)) * Extent.y
				+ Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Up)) * Extent.z;
			OutMinimum = Transform.Translation - WorldExtent;
			OutMaximum = Transform.Translation + WorldExtent;
			return true;
		}

		auto WorldBoundsToTargetLocal(
			const FVector3& WorldMinimum, const FVector3& WorldMaximum,
			const FTransform& TargetTransform,
			FVector3& OutMinimum, FVector3& OutMaximum) -> bool
		{
			if (!IsValidPhysicsTransform(TargetTransform)) return false;
			const FQuat InverseRotation = Math::Inverse(NormalizedRotation(TargetTransform.Rotation));
			OutMinimum = FVector3(std::numeric_limits<double>::max());
			OutMaximum = FVector3(-std::numeric_limits<double>::max());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 World{
					(Corner & 1) ? WorldMaximum.x : WorldMinimum.x,
					(Corner & 2) ? WorldMaximum.y : WorldMinimum.y,
					(Corner & 4) ? WorldMaximum.z : WorldMinimum.z};
				const FVector3 Local = Math::RotateVector(
					InverseRotation, World - TargetTransform.Translation) / TargetTransform.Scale3D;
				OutMinimum = Math::Min(OutMinimum, Local);
				OutMaximum = Math::Max(OutMaximum, Local);
			}
			return Math::IsFinite(OutMinimum) && Math::IsFinite(OutMaximum);
		}

		template<typename TVisitor>
		auto VisitBvh(
			const FCollisionGeometryRef& Target,
			const FVector3& QueryMinimum,
			const FVector3& QueryMaximum,
			FCollisionGeometryCounters* Counters,
			TVisitor&& Visitor) -> bool
		{
			if (Target.GetNodeCount() == 0) return false;
			std::array<uint32, 128> Stack{};
			uint32 StackCount = 1;
			Stack[0] = 0;
			while (StackCount > 0)
			{
				const FCollisionGeometryNode* Node = Target.GetNode(Stack[--StackCount]);
				if (!Node) return false;
				if (Counters) AddCounter(Counters->AssetNodeTests, 1, Counters->bOverflowed);
				if (Node->Maximum.x < QueryMinimum.x || Node->Minimum.x > QueryMaximum.x
					|| Node->Maximum.y < QueryMinimum.y || Node->Minimum.y > QueryMaximum.y
					|| Node->Maximum.z < QueryMinimum.z || Node->Minimum.z > QueryMaximum.z) continue;
				if (Node->IsLeaf())
				{
					if (Counters) AddCounter(Counters->AssetLeafTests, 1, Counters->bOverflowed);
							for (uint32 Offset = 0; Offset < Node->GetLeafCount(); ++Offset)
					{
						const uint32 Triangle = Target.GetLeafTriangle(Node->First + Offset);
						if (Triangle == std::numeric_limits<uint32>::max() || !Visitor(Triangle)) return false;
					}
					continue;
				}
				if (StackCount + 2 > Stack.size())
				{
					if (Counters) Counters->bOverflowed = true;
					return false;
				}
				Stack[StackCount++] = Node->CountOrSecond;
				Stack[StackCount++] = Node->First;
			}
			return true;
		}

		auto FeatureDistanceProduction(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			const FVector3& LocalMinimum, const FVector3& LocalMaximum,
			FLeafDistance& OutDistance, uint32& OutOrdinal,
			FCollisionGeometryCounters* Counters) -> bool
		{
			bool bFound = false;
			const bool bTraversalValid = VisitBvh(Target, LocalMinimum, LocalMaximum, Counters,
				[&](uint32 Index) {
					FVector3 A;
					FVector3 B;
					FVector3 C;
					uint32 Ordinal = 0;
					if (!GetWorldTriangle(Target, Index, TargetTransform, A, B, C, Ordinal))
						return false;
					FLeafDistance Candidate;
					if (!TriangleDistance(Query, QueryTransform, A, B, C, Candidate, Counters)) return false;
					if (!bFound || Candidate.Separation < OutDistance.Separation
						|| (Candidate.Separation == OutDistance.Separation && Ordinal < OutOrdinal))
					{
						OutDistance = Candidate;
						OutOrdinal = Ordinal;
						bFound = true;
					}
					return true;
				});
			return bTraversalValid && bFound;
		}

		auto FeatureOverlapProduction(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			FVector3 WorldMinimum;
			FVector3 WorldMaximum;
			FVector3 LocalMinimum;
			FVector3 LocalMaximum;
			if (!BuildShapeWorldBounds(Query, QueryTransform, WorldMinimum, WorldMaximum)
				|| !WorldBoundsToTargetLocal(WorldMinimum, WorldMaximum, TargetTransform,
					LocalMinimum, LocalMaximum)) return ECollisionQueryStatus::Invalid;
			FLeafDistance Distance;
			uint32 Ordinal = 0;
			if (!FeatureDistanceProduction(Query, QueryTransform, Target, TargetTransform,
				LocalMinimum, LocalMaximum, Distance, Ordinal, Counters)) return ECollisionQueryStatus::Miss;
			if (Distance.Separation >= -ContactTolerance) return ECollisionQueryStatus::Miss;
			OutHit = {};
			OutHit.Time = 0.0;
			OutHit.Location = QueryTransform.Translation;
			OutHit.ImpactPoint = Distance.TargetPoint;
			OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, FVectorConstants::Forward);
			OutHit.PenetrationDepth = -Distance.Separation;
			OutHit.bStartPenetrating = true;
			return ECollisionQueryStatus::Hit;
		}

		auto FeatureSweepProduction(
			const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			const ECollisionQueryStatus Initial = FeatureOverlapProduction(
				Query, QueryTransform, Target, TargetTransform, OutHit, Counters);
			if (Initial == ECollisionQueryStatus::Hit || Initial == ECollisionQueryStatus::Invalid) return Initial;
			FVector3 StartMinimum;
			FVector3 StartMaximum;
			if (!BuildShapeWorldBounds(Query, QueryTransform, StartMinimum, StartMaximum))
				return ECollisionQueryStatus::Invalid;
			const FVector3 WorldMinimum = Math::Min(StartMinimum, StartMinimum + Delta);
			const FVector3 WorldMaximum = Math::Max(StartMaximum, StartMaximum + Delta);
			FVector3 LocalMinimum;
			FVector3 LocalMaximum;
			if (!WorldBoundsToTargetLocal(WorldMinimum, WorldMaximum, TargetTransform,
				LocalMinimum, LocalMaximum)) return ECollisionQueryStatus::Invalid;
			double Time = 0.0;
			for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			{
				RecordGeometryWork(Counters, 0, 1);
				FTransform Moved = QueryTransform;
				Moved.Translation += Delta * Time;
				FLeafDistance Distance;
				uint32 Ordinal = 0;
				if (!FeatureDistanceProduction(Query, Moved, Target, TargetTransform,
					LocalMinimum, LocalMaximum, Distance, Ordinal, Counters)) return ECollisionQueryStatus::Miss;
				if (Distance.Separation <= ContactTolerance)
				{
					if (Math::Dot(Delta, Distance.Normal) >= -ContactTolerance)
						return ECollisionQueryStatus::Miss;
					OutHit = {};
					OutHit.Time = std::clamp(Time, 0.0, 1.0);
					OutHit.Location = Moved.Translation;
					OutHit.ImpactPoint = Distance.TargetPoint;
					OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, -Delta);
					return ECollisionQueryStatus::Hit;
				}
				const double ClosingSpeed = -Math::Dot(Delta, Distance.Normal);
				if (ClosingSpeed <= ContactTolerance) return ECollisionQueryStatus::Miss;
				const double Step = Distance.Separation / ClosingSpeed;
				if (!std::isfinite(Step) || Step <= 1.0e-12) return ECollisionQueryStatus::NonConverged;
				Time += Step;
				if (Time > 1.0 + ContactTolerance) return ECollisionQueryStatus::Miss;
			}
			return ECollisionQueryStatus::NonConverged;
		}

		auto RaycastFeatureProduction(
			const FVector3& Start, const FVector3& End,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			const FQuat InverseRotation = Math::Inverse(NormalizedRotation(TargetTransform.Rotation));
			const FVector3 LocalStart = Math::RotateVector(
				InverseRotation, Start - TargetTransform.Translation) / TargetTransform.Scale3D;
			const FVector3 LocalEnd = Math::RotateVector(
				InverseRotation, End - TargetTransform.Translation) / TargetTransform.Scale3D;
			const FVector3 LocalDelta = LocalEnd - LocalStart;
			auto NodeInterval = [&](const FCollisionGeometryNode& Node, double& OutNear) {
				double Near = 0.0;
				double Far = 1.0;
				for (uint32 Axis = 0; Axis < 3; ++Axis)
				{
					if (std::abs(LocalDelta[Axis]) <= ContactTolerance)
					{
						if (LocalStart[Axis] < Node.Minimum[Axis]
							|| LocalStart[Axis] > Node.Maximum[Axis]) return false;
						continue;
					}
					double First = (Node.Minimum[Axis] - LocalStart[Axis]) / LocalDelta[Axis];
					double Second = (Node.Maximum[Axis] - LocalStart[Axis]) / LocalDelta[Axis];
					if (First > Second) std::swap(First, Second);
					Near = std::max(Near, First);
					Far = std::min(Far, Second);
					if (Near > Far) return false;
				}
				OutNear = Near;
				return Far >= 0.0 && Near <= 1.0;
			};
			struct FStackEntry { uint32 Node = 0; double Near = 0.0; };
			std::array<FStackEntry, 128> Stack{};
			uint32 StackCount = 0;
			const FCollisionGeometryNode* Root = Target.GetNode(0);
			double RootNear = 0.0;
			if (!Root) return ECollisionQueryStatus::Invalid;
			if (!NodeInterval(*Root, RootNear))
			{
				if (Counters) AddCounter(Counters->AssetNodeTests, 1, Counters->bOverflowed);
				return ECollisionQueryStatus::Miss;
			}
			Stack[StackCount++] = {0, RootNear};
			bool bFound = false;
			uint32 Winner = 0;
			while (StackCount > 0)
			{
				const FStackEntry Entry = Stack[--StackCount];
				if (bFound && Entry.Near > OutHit.Time) continue;
				const FCollisionGeometryNode* Node = Target.GetNode(Entry.Node);
				if (!Node) return ECollisionQueryStatus::Invalid;
				if (Counters) AddCounter(Counters->AssetNodeTests, 1, Counters->bOverflowed);
				if (Node->IsLeaf())
				{
					if (Counters) AddCounter(Counters->AssetLeafTests, 1, Counters->bOverflowed);
					auto TestTriangle = [&](uint32 Index) {
						FVector3 A;
						FVector3 B;
						FVector3 C;
						uint32 Ordinal = 0;
						if (!GetWorldTriangle(Target, Index, TargetTransform, A, B, C, Ordinal))
							return false;
						if (Counters)
						{
							AddCounter(Counters->FeatureTests, 1, Counters->bOverflowed);
							AddCounter(Counters->LeafTests, 1, Counters->bOverflowed);
						}
						FPhysicsQueryHit Candidate;
						if (RaycastTriangle(Start, End, A, B, C, Candidate)
							&& (!bFound || Candidate.Time < OutHit.Time
								|| (Candidate.Time == OutHit.Time && Ordinal < Winner)))
						{
							OutHit = Candidate;
							Winner = Ordinal;
							bFound = true;
						}
						return true;
					};
					for (uint32 Offset = 0; Offset < Node->GetLeafCount(); ++Offset)
							if (!TestTriangle(Target.GetLeafTriangle(Node->First + Offset)))
								return ECollisionQueryStatus::Invalid;
					continue;
				}
				const FCollisionGeometryNode* Left = Target.GetNode(Node->First);
				const FCollisionGeometryNode* Right = Target.GetNode(Node->CountOrSecond);
				if (!Left || !Right) return ECollisionQueryStatus::Invalid;
				double LeftNear = 0.0;
				double RightNear = 0.0;
				const bool bLeft = NodeInterval(*Left, LeftNear) && (!bFound || LeftNear <= OutHit.Time);
				const bool bRight = NodeInterval(*Right, RightNear) && (!bFound || RightNear <= OutHit.Time);
				if (StackCount + static_cast<uint32>(bLeft) + static_cast<uint32>(bRight) > Stack.size())
				{
					if (Counters) Counters->bOverflowed = true;
					return ECollisionQueryStatus::Unsupported;
				}
				if (bLeft && bRight)
				{
					if (LeftNear <= RightNear)
					{
						Stack[StackCount++] = {Node->CountOrSecond, RightNear};
						Stack[StackCount++] = {Node->First, LeftNear};
					}
					else
					{
						Stack[StackCount++] = {Node->First, LeftNear};
						Stack[StackCount++] = {Node->CountOrSecond, RightNear};
					}
				}
				else if (bLeft) Stack[StackCount++] = {Node->First, LeftNear};
				else if (bRight) Stack[StackCount++] = {Node->CountOrSecond, RightNear};
			}
			return bFound ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
		}

		auto RaycastFeature(
			const FVector3& Start, const FVector3& End,
			const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			bool bFound = false;
			uint32 Winner = 0;
			for (uint32 Index = 0; Index < Target.GetTriangleCount(); ++Index)
			{
				FVector3 A;
				FVector3 B;
				FVector3 C;
				uint32 Ordinal = 0;
				if (!GetWorldTriangle(Target, Index, TargetTransform, A, B, C, Ordinal))
					return ECollisionQueryStatus::Invalid;
				if (Counters)
				{
					AddCounter(Counters->FeatureTests, 1, Counters->bOverflowed);
					AddCounter(Counters->LeafTests, 1, Counters->bOverflowed);
				}
				FPhysicsQueryHit Candidate;
				if (!RaycastTriangle(Start, End, A, B, C, Candidate)
					|| (bFound && (Candidate.Time > OutHit.Time
						|| (Candidate.Time == OutHit.Time && Ordinal >= Winner)))) continue;
				OutHit = Candidate;
				Winner = Ordinal;
				bFound = true;
			}
			return bFound ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
		}

		auto RaycastSphereLeaf(
			const FVector3& Start, const FVector3& End,
			const FCollisionShape& Sphere, const FTransform& Transform, FPhysicsQueryHit& OutHit) -> bool
		{
			FVector3 Center;
			double Radius = 0.0;
			if (!GetSphere(Sphere, Transform, Center, Radius)) return false;
			const FVector3 Delta = End - Start;
			const FVector3 Offset = Start - Center;
			const double C = Math::Dot(Offset, Offset) - Radius * Radius;
			if (C < -ContactTolerance)
			{
				OutHit.Time = 0.0;
				OutHit.Location = Start;
				OutHit.bStartPenetrating = true;
				const double CenterDistance = Math::Length(Offset);
				OutHit.ImpactNormal = Math::NormalizeOr(Offset, FVectorConstants::Forward);
				OutHit.ImpactPoint = Center + OutHit.ImpactNormal * Radius;
				OutHit.PenetrationDepth = Radius - CenterDistance;
				return true;
			}
			const double A = Math::Dot(Delta, Delta);
			const double B = Math::Dot(Offset, Delta);
			const double Discriminant = B * B - A * C;
			if (A <= ContactTolerance || Discriminant < 0.0) return false;
			const double Time = (-B - std::sqrt(std::max(0.0, Discriminant))) / A;
			if (Time < 0.0 || Time > 1.0) return false;
			OutHit.Time = Time;
			OutHit.Location = Start + Delta * Time;
			OutHit.ImpactNormal = Math::NormalizeOr(OutHit.Location - Center, FVectorConstants::Forward);
			OutHit.ImpactPoint = OutHit.Location;
			return true;
		}

		auto RaycastCapsuleLeaf(
			const FVector3& Start, const FVector3& End,
			const FCollisionShape& Capsule, const FTransform& Transform, FPhysicsQueryHit& OutHit) -> bool
		{
			FVector3 SegmentStart;
			FVector3 SegmentEnd;
			double Radius = 0.0;
			if (!GetCapsuleSegment(Capsule, Transform, SegmentStart, SegmentEnd, Radius)) return false;
			const FVector3 Closest = ClosestPointOnSegment(Start, SegmentStart, SegmentEnd);
			const FVector3 InitialDelta = Start - Closest;
			const double InitialDistance = Math::Length(InitialDelta);
			if (InitialDistance < Radius - ContactTolerance)
			{
				OutHit.Time = 0.0;
				OutHit.Location = Start;
				OutHit.ImpactNormal = Math::NormalizeOr(InitialDelta, FVectorConstants::Forward);
				OutHit.ImpactPoint = Closest + OutHit.ImpactNormal * Radius;
				OutHit.PenetrationDepth = Radius - InitialDistance;
				OutHit.bStartPenetrating = true;
				return true;
			}
			const FVector3 Delta = End - Start;
			const double Length = Math::Length(Delta);
			if (Length <= ContactTolerance) return false;
			const FVector3 Direction = Delta / Length;
			const FVector3 Axis = SegmentEnd - SegmentStart;
			const FVector3 Offset = Start - SegmentStart;
			const double AxisSquared = Math::Dot(Axis, Axis);
			const double AxisDirection = Math::Dot(Axis, Direction);
			const double AxisOffset = Math::Dot(Axis, Offset);
			const double DirectionOffset = Math::Dot(Direction, Offset);
			const double OffsetSquared = Math::Dot(Offset, Offset);
			const double A = AxisSquared - AxisDirection * AxisDirection;
			const double B = AxisSquared * DirectionOffset - AxisOffset * AxisDirection;
			const double C = AxisSquared * OffsetSquared - AxisOffset * AxisOffset
				- Radius * Radius * AxisSquared;
			double Distance = std::numeric_limits<double>::max();
			const double Discriminant = B * B - A * C;
			if (std::abs(A) > ContactTolerance && Discriminant >= 0.0)
			{
				const double Candidate = (-B - std::sqrt(std::max(0.0, Discriminant))) / A;
				const double AxisTime = AxisOffset + Candidate * AxisDirection;
				if (Candidate >= 0.0 && AxisTime >= 0.0 && AxisTime <= AxisSquared) Distance = Candidate;
			}
			for (const FVector3& CapCenter : {SegmentStart, SegmentEnd})
			{
				FPhysicsQueryHit CapHit;
				FTransform SphereTransform;
				SphereTransform.Translation = CapCenter;
				if (!RaycastSphereLeaf(Start, End, FCollisionShape::MakeSphere(Radius), SphereTransform, CapHit)) continue;
				Distance = std::min(Distance, CapHit.Time * Length);
			}
			if (!std::isfinite(Distance) || Distance < 0.0 || Distance > Length) return false;
			OutHit.Time = Distance / Length;
			OutHit.Location = Start + Delta * OutHit.Time;
			const FVector3 AxisPoint = ClosestPointOnSegment(OutHit.Location, SegmentStart, SegmentEnd);
			OutHit.ImpactNormal = Math::NormalizeOr(OutHit.Location - AxisPoint, -Direction);
			OutHit.ImpactPoint = OutHit.Location;
			return true;
		}

		auto OverlapLeaf(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> bool
		{
			FLeafDistance Distance;
			if (!PrimitiveDistance(Query, QueryTransform, Target, TargetTransform, Distance, Counters)
				|| Distance.Separation >= -ContactTolerance) return false;
			OutHit = {};
			OutHit.Time = 0.0;
			OutHit.Location = QueryTransform.Translation;
			OutHit.ImpactPoint = Distance.TargetPoint;
			OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, FVectorConstants::Forward);
			OutHit.PenetrationDepth = -Distance.Separation;
			OutHit.bStartPenetrating = true;
			return true;
		}

		auto SweepLeaf(
			const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			if (OverlapLeaf(Query, QueryTransform, Target, TargetTransform, OutHit, Counters)) return ECollisionQueryStatus::Hit;
			if (Math::LengthSquared(Delta) <= ContactTolerance * ContactTolerance) return ECollisionQueryStatus::Miss;
			double Time = 0.0;
			for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			{
				RecordGeometryWork(Counters, 0, 1);
				FTransform Moved = QueryTransform;
				Moved.Translation += Delta * Time;
				FLeafDistance Distance;
				if (!PrimitiveDistance(Query, Moved, Target, TargetTransform, Distance, Counters))
					return ECollisionQueryStatus::Unsupported;
				if (Distance.Separation <= ContactTolerance)
				{
					if (Distance.Separation >= -ContactTolerance
						&& Math::Dot(Delta, Distance.Normal) >= -ContactTolerance)
						return ECollisionQueryStatus::Miss;
					OutHit = {};
					OutHit.Time = std::clamp(Time, 0.0, 1.0);
					OutHit.Location = Moved.Translation;
					OutHit.ImpactPoint = Distance.TargetPoint;
					OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, -Delta);
					return ECollisionQueryStatus::Hit;
				}
				const double ClosingSpeed = -Math::Dot(Delta, Distance.Normal);
				if (ClosingSpeed <= ContactTolerance) return ECollisionQueryStatus::Miss;
				const double Step = Distance.Separation / ClosingSpeed;
				if (!std::isfinite(Step) || Step <= 1.0e-12) return ECollisionQueryStatus::NonConverged;
				Time += Step;
				if (Time > 1.0 + ContactTolerance) return ECollisionQueryStatus::Miss;
			}
			return ECollisionQueryStatus::NonConverged;
		}

		auto AddSimpleCounter(uint64& Value, FCollisionGeometryCounters* Counters) -> void
		{
			if (!Counters) return;
			AddCounter(Value, 1, Counters->bOverflowed);
		}
	}

	auto Raycast(
		const FVector3& Start, const FVector3& End,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Math::IsFinite(Start) || !Math::IsFinite(End) || !Target.IsValid()
			|| !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		if (Target.GetKind() == ECollisionGeometryKind::ConvexHull
			|| Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
		{
			if (Counters) AddSimpleCounter(Counters->GenericDispatches, Counters);
			if (Algorithm == ECollisionQueryAlgorithm::Production
				&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
				&& Target.GetNodeCount() > 0)
			{
				const ECollisionQueryStatus Status = RaycastFeatureProduction(
					Start, End, Target, TargetTransform, OutHit, Counters);
				if (Status != ECollisionQueryStatus::Unsupported) return Status;
				if (Counters) AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
			}
			else if (Algorithm == ECollisionQueryAlgorithm::Production
				&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh) && Counters)
				AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
			return RaycastFeature(Start, End, Target, TargetTransform, OutHit, Counters);
		}
		bool bFound = false;
		uint32 Winner = 0;
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->AnalyticDispatches, Counters);
			}
			FPhysicsQueryHit Candidate;
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			bool bHit = false;
			switch (Child->Shape.GetType())
			{
			case ECollisionShapeType::Box:
				bHit = RaycastBox(Start, End, Child->Shape, ChildTransform, Candidate, Counters);
				break;
			case ECollisionShapeType::Sphere:
				bHit = RaycastSphereLeaf(Start, End, Child->Shape, ChildTransform, Candidate);
				break;
			case ECollisionShapeType::Capsule:
				bHit = RaycastCapsuleLeaf(Start, End, Child->Shape, ChildTransform, Candidate);
				break;
			}
			if (!bHit || (bFound && (Candidate.Time > OutHit.Time
				|| (Candidate.Time == OutHit.Time && Index >= Winner)))) continue;
			OutHit = Candidate;
			Winner = Index;
			bFound = true;
		}
		return bFound ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
	}

	auto Sweep(
		const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Query.IsValid() || !IsValidPhysicsTransform(QueryTransform) || !Math::IsFinite(Delta)
			|| !Target.IsValid() || !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		if (Target.GetKind() == ECollisionGeometryKind::ConvexHull
			|| Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
		{
			if (Counters) AddSimpleCounter(Counters->GenericDispatches, Counters);
			ECollisionQueryStatus Status;
			if (Algorithm == ECollisionQueryAlgorithm::Production
				&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
				&& Target.GetNodeCount() > 0)
			{
				Status = FeatureSweepProduction(
					Query, QueryTransform, Delta, Target, TargetTransform, OutHit, Counters);
				if (Status == ECollisionQueryStatus::Unsupported)
				{
					if (Counters) AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
					Status = FeatureSweep(
						Query, QueryTransform, Delta, Target, TargetTransform, OutHit, Counters);
				}
			}
			else
			{
				if (Algorithm == ECollisionQueryAlgorithm::Production
					&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh) && Counters)
					AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
				Status = FeatureSweep(
					Query, QueryTransform, Delta, Target, TargetTransform, OutHit, Counters);
			}
			if (Counters && Status == ECollisionQueryStatus::NonConverged)
				AddSimpleCounter(Counters->NonConverged, Counters);
			if (Counters && Status == ECollisionQueryStatus::Unsupported)
				AddSimpleCounter(Counters->Unsupported, Counters);
			return Status;
		}
		ECollisionQueryStatus FinalStatus = ECollisionQueryStatus::Miss;
		uint32 Winner = 0;
		bool bFound = false;
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->GenericDispatches, Counters);
			}
			FPhysicsQueryHit Candidate;
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			ECollisionQueryStatus Status = SweepLeaf(
				Query, QueryTransform, Delta, Child->Shape, ChildTransform, Candidate, Counters);
			if ((Status == ECollisionQueryStatus::NonConverged
				|| Status == ECollisionQueryStatus::Unsupported)
				&& Algorithm == ECollisionQueryAlgorithm::Production
				&& Query.GetType() == ECollisionShapeType::Capsule
				&& Child->Shape.GetType() == ECollisionShapeType::Box)
			{
				if (Counters) AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
				Status = SweepCapsuleBox(Query, QueryTransform, Delta, Child->Shape, ChildTransform,
					Candidate, Counters) ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
			}
			if (Status == ECollisionQueryStatus::NonConverged)
			{
				FinalStatus = Status;
				if (Counters) AddSimpleCounter(Counters->NonConverged, Counters);
				continue;
			}
			if (Status == ECollisionQueryStatus::Unsupported)
			{
				FinalStatus = Status;
				if (Counters) AddSimpleCounter(Counters->Unsupported, Counters);
				continue;
			}
			if (Status != ECollisionQueryStatus::Hit || (bFound && (Candidate.Time > OutHit.Time
				|| (Candidate.Time == OutHit.Time && Index >= Winner)))) continue;
			OutHit = Candidate;
			Winner = Index;
			bFound = true;
			FinalStatus = ECollisionQueryStatus::Hit;
		}
		return FinalStatus;
	}

	auto Overlap(
		const FCollisionShape& Query, const FTransform& QueryTransform,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Query.IsValid() || !IsValidPhysicsTransform(QueryTransform)
			|| !Target.IsValid() || !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		if (Target.GetKind() == ECollisionGeometryKind::ConvexHull
			|| Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
		{
			if (Counters) AddSimpleCounter(Counters->GenericDispatches, Counters);
			if (Algorithm == ECollisionQueryAlgorithm::Production
				&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh)
				&& Target.GetNodeCount() > 0)
				return FeatureOverlapProduction(
					Query, QueryTransform, Target, TargetTransform, OutHit, Counters);
			if (Algorithm == ECollisionQueryAlgorithm::Production
				&& (Target.GetKind() == ECollisionGeometryKind::TriangleMesh) && Counters)
				AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
			return FeatureOverlap(Query, QueryTransform, Target, TargetTransform, OutHit, Counters)
				? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
		}
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->AnalyticDispatches, Counters);
			}
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			const bool bHit = OverlapLeaf(
				Query, QueryTransform, Child->Shape, ChildTransform, OutHit, Counters);
			if (bHit)
				return ECollisionQueryStatus::Hit;
		}
		return ECollisionQueryStatus::Miss;
	}
}
