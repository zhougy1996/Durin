#include <gtest/gtest.h>

#include "DObject/ObjectLifecycle.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshSourceTranslation.h"
#include "StaticMesh/StaticMeshBuildDerivedData.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshResources.h"

#define DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(Suite, Name) GTEST_TEST(Suite, Name)

namespace
{
	using namespace Durin;

	inline constexpr uint32 CollisionPayloadMagic = 0x4c4f4344; // DCOL
	inline constexpr uint32 CollisionPayloadSchemaVersion = 1;
	inline constexpr uint32 CollisionBuilderVersion = 1;
	inline constexpr uint32 CollisionKeySchemaVersion = 1;
	inline constexpr uint32 CollisionPayloadAlignment = 16;
	inline constexpr uint32 CollisionPayloadHeaderSize = 64;
	inline constexpr uint32 CollisionPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumCollisionPayloadChunks = 8;
	inline constexpr uint64 MaximumCollisionPayloadBytes = 256ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumCollisionBuilderPeakBytes = 512ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumCollisionTriangles = 2'000'000;
	inline constexpr uint32 MaximumConvexHullVertices = 256;
	inline constexpr uint32 CollisionTrianglesPerLeaf = 8;
	inline constexpr uint32 MaximumCollisionTreeDepth = 64;
	inline constexpr uint32 CollisionTraversalStackEntries = 128;
	inline constexpr uint32 MaximumCollisionDebugTriangles = 256;
	inline constexpr uint32 LeafFlag = 0x80000000u;
	inline const FGuid StaticMeshCollisionPayloadId{
		0x3c10f7d1, 0x92fa4e20, 0xb544ad79, 0x1d788064};

	auto EncodeCollisionPayload(
		const FStaticMeshCollisionPayloadData& Payload,
		EStaticMeshTargetPlatform Platform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Candidate;
		FCanonicalMemoryWriter Ar(Candidate, EArchivePurpose::DerivedDataPayload);
		const_cast<FStaticMeshCollisionPayloadData&>(Payload).Serialize(Ar, Platform);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto DecodeCollisionPayload(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform Platform,
		FStaticMeshCollisionPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		FStaticMeshCollisionPayloadData Candidate;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, Platform);
		if (Ar.HasError())
			return {Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
				? EPayloadDecodeError::Incompatible : EPayloadDecodeError::Corrupt,
				Ar.GetFailure()->Message};
		OutPayload = std::move(Candidate);
		return {};
	}

	enum class ECollisionSourceMode : uint8
	{
		None,
		ConvexHullFromLOD0,
		TriangleMeshFromLOD0
	};

	enum class ECollisionQueryPolicy : uint8
	{
		SimpleOnly,
		ComplexOnly,
		SimpleAndComplex
	};

	enum class ECollisionQueryComplexity : uint8
	{
		Default,
		Simple,
		Complex
	};

	enum class ECollisionRepresentation : uint8
	{
		None,
		Simple,
		Complex
	};

	enum class EPrototypeOperation : uint8
	{
		Ray,
		Sweep,
		Overlap
	};

	enum class EPrototypeQueryShape : uint8
	{
		Box,
		Sphere,
		Capsule
	};

	enum class EPrototypeTarget : uint8
	{
		ConvexHull,
		TriangleMesh
	};

	enum class EPrototypeAlgorithm : uint8
	{
		HullPlaneClip,
		SupportMapping,
		DoubleSidedRayTriangle,
		ClosestTriangleFeature,
		BoxTriangleSAT,
		BoundedFeatureAdvancement
	};

	struct FCollisionMeshNodePrototype
	{
		FVector3f Minimum;
		uint32 First = 0;
		FVector3f Maximum;
		uint32 CountOrSecond = 0;

		auto IsLeaf() const -> bool { return (CountOrSecond & LeafFlag) != 0; }
		auto GetLeafCount() const -> uint32 { return CountOrSecond & ~LeafFlag; }
	};

	struct FCollisionHullPlanePrototype
	{
		FVector3f Normal;
		float Distance = 0.0f;
	};

	struct FCollisionHullHalfEdgePrototype
	{
		uint32 Origin = 0;
		uint32 Twin = 0;
		uint32 Next = 0;
		uint32 Face = 0;
	};

	struct FCollisionHullFacePrototype
	{
		uint32 FirstEdge = 0;
		uint32 EdgeCount = 0;
		uint32 SourceOrdinal = 0;
		uint32 Reserved = 0;
	};

	static_assert(sizeof(FCollisionMeshNodePrototype) == 32);
	static_assert(sizeof(FCollisionHullPlanePrototype) == 16);
	static_assert(sizeof(FCollisionHullHalfEdgePrototype) == 16);
	static_assert(sizeof(FCollisionHullFacePrototype) == 16);

	struct FCollisionSourceFixture
	{
		std::string Name;
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
	};

	struct FTriangleBuildRecord
	{
		uint32 Ordinal = 0;
		std::array<float, 3> Centroid{};
		std::array<float, 3> Minimum{};
		std::array<float, 3> Maximum{};
	};

	struct FMeshBuildFacts
	{
		std::vector<FCollisionMeshNodePrototype> Nodes;
		std::vector<uint32> TriangleOrdinals;
		uint32 SourceTriangles = 0;
		uint32 RetainedTriangles = 0;
		uint32 RemovedTriangles = 0;
		uint32 MaximumDepth = 0;
		uint64 LogicalRetainedBytes = 0;
	};

	struct FReferenceEvidence
	{
		EPrototypeTarget Target;
		EPrototypeOperation Operation;
		EPrototypeQueryShape Query;
		double Time;
		FVector3 Normal;
		bool bHit;
	};

	auto MakeReferenceEvidence() -> std::vector<FReferenceEvidence>
	{
		std::vector<FReferenceEvidence> Result;
		for (EPrototypeTarget Target : {EPrototypeTarget::ConvexHull, EPrototypeTarget::TriangleMesh})
		{
			for (EPrototypeOperation Operation : {EPrototypeOperation::Ray,
				EPrototypeOperation::Sweep, EPrototypeOperation::Overlap})
			{
				for (EPrototypeQueryShape Query : {EPrototypeQueryShape::Box,
					EPrototypeQueryShape::Sphere, EPrototypeQueryShape::Capsule})
				{
					double Time = 0.0;
					if (Operation == EPrototypeOperation::Ray)
						Time = Target == EPrototypeTarget::ConvexHull ? 1.0 / 3.0 : 0.5;
					else if (Operation == EPrototypeOperation::Sweep)
						Time = Target == EPrototypeTarget::ConvexHull ? 1.75 / 6.0
							: Query == EPrototypeQueryShape::Capsule ? 0.25 : 0.375;
					Result.push_back({Target, Operation, Query, Time,
						Target == EPrototypeTarget::ConvexHull ? FVector3(-1, 0, 0)
							: FVector3(0, 0, 1), true});
				}
			}
		}
		return Result;
	}

	auto ResolveRepresentation(
		ECollisionQueryPolicy Policy,
		ECollisionQueryComplexity Complexity,
		bool bHasSimple,
		bool bHasComplex) -> ECollisionRepresentation
	{
		if (Complexity == ECollisionQueryComplexity::Simple)
			return bHasSimple && Policy != ECollisionQueryPolicy::ComplexOnly
				? ECollisionRepresentation::Simple : ECollisionRepresentation::None;
		if (Complexity == ECollisionQueryComplexity::Complex)
			return bHasComplex && Policy != ECollisionQueryPolicy::SimpleOnly
				? ECollisionRepresentation::Complex : ECollisionRepresentation::None;
		if (Policy != ECollisionQueryPolicy::ComplexOnly && bHasSimple)
			return ECollisionRepresentation::Simple;
		if (Policy != ECollisionQueryPolicy::SimpleOnly && bHasComplex)
			return ECollisionRepresentation::Complex;
		return ECollisionRepresentation::None;
	}

	auto SelectAlgorithm(
		EPrototypeOperation Operation,
		EPrototypeQueryShape Query,
		EPrototypeTarget Target) -> EPrototypeAlgorithm
	{
		if (Target == EPrototypeTarget::ConvexHull)
			return Operation == EPrototypeOperation::Ray
				? EPrototypeAlgorithm::HullPlaneClip
				: EPrototypeAlgorithm::SupportMapping;
		if (Operation == EPrototypeOperation::Ray)
			return EPrototypeAlgorithm::DoubleSidedRayTriangle;
		if (Operation == EPrototypeOperation::Sweep)
			return EPrototypeAlgorithm::BoundedFeatureAdvancement;
		return Query == EPrototypeQueryShape::Box
			? EPrototypeAlgorithm::BoxTriangleSAT
			: EPrototypeAlgorithm::ClosestTriangleFeature;
	}

	auto MakeTetrahedron() -> FCollisionSourceFixture
	{
		return {
			"Tetrahedron",
			{{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}},
			{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}};
	}

	auto MakeCube() -> FCollisionSourceFixture
	{
		return {
			"Cube",
			{{-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {-1, 1, 1},
			 {1, -1, -1}, {1, -1, 1}, {1, 1, -1}, {1, 1, 1}},
			{0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5,
			 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6,
			 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3}};
	}

	auto MakeEdgeCaseFixtures() -> std::vector<FCollisionSourceFixture>
	{
		FCollisionSourceFixture Open{"Open", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}},
			{0, 1, 2, 0, 2, 3}};
		FCollisionSourceFixture NonManifold{"NonManifold",
			{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}},
			{0, 1, 2, 1, 0, 3, 0, 1, 4}};
		FCollisionSourceFixture Duplicate{"DuplicateAndDegenerate",
			{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2, 0, 1, 2, 0, 0, 2}};
		FCollisionSourceFixture Thin{"ThinLargeCoordinate",
			{{1.0e6f, 0, 0}, {1.0e6f + 64.0f, 0, 0}, {1.0e6f, 0.001f, 0}}, {0, 1, 2}};
		FCollisionSourceFixture Reversed = MakeCube();
		Reversed.Name = "ReversedWinding";
		for (size_t Index = 0; Index < Reversed.Indices.size(); Index += 3)
			std::swap(Reversed.Indices[Index + 1], Reversed.Indices[Index + 2]);
		FCollisionSourceFixture Clustered{"Clustered", {}, {}};
		for (uint32 Index = 0; Index < 96; ++Index)
		{
			const float Offset = static_cast<float>(Index) * 1.0e-4f;
			const uint32 Base = static_cast<uint32>(Clustered.Positions.size());
			Clustered.Positions.insert(Clustered.Positions.end(),
				{{Offset, 0, 0}, {Offset + 0.01f, 0, 0}, {Offset, 0.01f, 0}});
			Clustered.Indices.insert(Clustered.Indices.end(), {Base, Base + 1, Base + 2});
		}
		return {MakeTetrahedron(), MakeCube(), std::move(Open), std::move(NonManifold),
			std::move(Duplicate), std::move(Thin), std::move(Reversed), std::move(Clustered)};
	}

	auto IsFinite(const FVector3f& Value) -> bool
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
	}

	auto IsAcceptedPhysicsScale(const FVector3& Scale) -> bool
	{
		return Math::IsFinite(Scale) && Scale.x > 0.0 && Scale.y > 0.0 && Scale.z > 0.0;
	}

	auto ValidateClosedHullInput(const FCollisionSourceFixture& Fixture) -> bool
	{
		if (Fixture.Positions.size() < 4 || Fixture.Positions.size() > MaximumConvexHullVertices
			|| Fixture.Indices.empty() || Fixture.Indices.size() % 3 != 0) return false;
		std::unordered_map<uint64, std::pair<uint32, int32>> Edges;
		for (size_t Index = 0; Index < Fixture.Indices.size(); Index += 3)
		{
			const std::array<uint32, 3> Triangle{
				Fixture.Indices[Index], Fixture.Indices[Index + 1], Fixture.Indices[Index + 2]};
			if (Triangle[0] >= Fixture.Positions.size() || Triangle[1] >= Fixture.Positions.size()
				|| Triangle[2] >= Fixture.Positions.size()) return false;
			for (size_t EdgeIndex = 0; EdgeIndex < 3; ++EdgeIndex)
			{
				const uint32 A = Triangle[EdgeIndex];
				const uint32 B = Triangle[(EdgeIndex + 1) % 3];
				if (A == B) return false;
				const uint32 Minimum = std::min(A, B);
				const uint32 Maximum = std::max(A, B);
				auto& [Count, Balance] = Edges[(static_cast<uint64>(Minimum) << 32) | Maximum];
				++Count;
				Balance += A == Minimum ? 1 : -1;
			}
		}
		if (!std::ranges::all_of(Edges, [](const auto& Item) {
			return Item.second.first == 2 && Item.second.second == 0;
		})) return false;
		const FVector3f A = Fixture.Positions[0];
		for (size_t BIndex = 1; BIndex + 2 < Fixture.Positions.size(); ++BIndex)
			for (size_t CIndex = BIndex + 1; CIndex + 1 < Fixture.Positions.size(); ++CIndex)
				for (size_t DIndex = CIndex + 1; DIndex < Fixture.Positions.size(); ++DIndex)
				{
					const FVector3f B = Fixture.Positions[BIndex];
					const FVector3f C = Fixture.Positions[CIndex];
					const FVector3f D = Fixture.Positions[DIndex];
					const double Volume6 =
						static_cast<double>(B.x - A.x) * ((C.y - A.y) * (D.z - A.z) - (C.z - A.z) * (D.y - A.y))
						- static_cast<double>(B.y - A.y) * ((C.x - A.x) * (D.z - A.z) - (C.z - A.z) * (D.x - A.x))
						+ static_cast<double>(B.z - A.z) * ((C.x - A.x) * (D.y - A.y) - (C.y - A.y) * (D.x - A.x));
					if (std::abs(Volume6) > 1.0e-12) return true;
				}
		return false;
	}

	auto BuildTriangleRecords(
		const FCollisionSourceFixture& Fixture,
		std::vector<FTriangleBuildRecord>& OutRecords,
		uint32& OutRemoved) -> bool
	{
		if (Fixture.Indices.empty() || Fixture.Indices.size() % 3 != 0
			|| Fixture.Indices.size() / 3 > MaximumCollisionTriangles) return false;
		for (const FVector3f& Position : Fixture.Positions) if (!IsFinite(Position)) return false;
		std::vector<FTriangleBuildRecord> Candidate;
		Candidate.reserve(Fixture.Indices.size() / 3);
		uint32 Removed = 0;
		for (uint32 Ordinal = 0; Ordinal < Fixture.Indices.size() / 3; ++Ordinal)
		{
			const uint32 IA = Fixture.Indices[Ordinal * 3];
			const uint32 IB = Fixture.Indices[Ordinal * 3 + 1];
			const uint32 IC = Fixture.Indices[Ordinal * 3 + 2];
			if (IA >= Fixture.Positions.size() || IB >= Fixture.Positions.size()
				|| IC >= Fixture.Positions.size()) return false;
			const FVector3f A = Fixture.Positions[IA];
			const FVector3f B = Fixture.Positions[IB];
			const FVector3f C = Fixture.Positions[IC];
			const float ABX = B.x - A.x, ABY = B.y - A.y, ABZ = B.z - A.z;
			const float ACX = C.x - A.x, ACY = C.y - A.y, ACZ = C.z - A.z;
			const float X = ABY * ACZ - ABZ * ACY;
			const float Y = ABZ * ACX - ABX * ACZ;
			const float Z = ABX * ACY - ABY * ACX;
			if (IA == IB || IB == IC || IC == IA || X * X + Y * Y + Z * Z <= 1.0e-20f)
			{
				++Removed;
				continue;
			}
			FTriangleBuildRecord Record;
			Record.Ordinal = Ordinal;
			Record.Centroid = {(A.x + B.x + C.x) / 3.0f,
				(A.y + B.y + C.y) / 3.0f, (A.z + B.z + C.z) / 3.0f};
			Record.Minimum = {std::min({A.x, B.x, C.x}), std::min({A.y, B.y, C.y}),
				std::min({A.z, B.z, C.z})};
			Record.Maximum = {std::max({A.x, B.x, C.x}), std::max({A.y, B.y, C.y}),
				std::max({A.z, B.z, C.z})};
			Candidate.push_back(Record);
		}
		if (Candidate.empty()) return false;
		OutRecords = std::move(Candidate);
		OutRemoved = Removed;
		return true;
	}

	auto BuildMeshPrototype(
		const FCollisionSourceFixture& Fixture,
		FMeshBuildFacts& OutFacts) -> bool
	{
		std::vector<FTriangleBuildRecord> Records;
		uint32 Removed = 0;
		if (!BuildTriangleRecords(Fixture, Records, Removed)) return false;
		FMeshBuildFacts Candidate;
		Candidate.SourceTriangles = static_cast<uint32>(Fixture.Indices.size() / 3);
		Candidate.RetainedTriangles = static_cast<uint32>(Records.size());
		Candidate.RemovedTriangles = Removed;
		Candidate.Nodes.reserve(Records.size() * 2 / CollisionTrianglesPerLeaf + 1);
		Candidate.TriangleOrdinals.reserve(Records.size());

		std::function<uint32(size_t, size_t, uint32)> BuildRange =
			[&](size_t Begin, size_t End, uint32 Depth) -> uint32
		{
			Candidate.MaximumDepth = std::max(Candidate.MaximumDepth, Depth);
			const uint32 NodeIndex = static_cast<uint32>(Candidate.Nodes.size());
			Candidate.Nodes.emplace_back();
			std::array<float, 3> Minimum{
				std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity()};
			std::array<float, 3> Maximum{-Minimum[0], -Minimum[1], -Minimum[2]};
			for (size_t Index = Begin; Index < End; ++Index)
				for (size_t Axis = 0; Axis < 3; ++Axis)
				{
					Minimum[Axis] = std::min(Minimum[Axis], Records[Index].Minimum[Axis]);
					Maximum[Axis] = std::max(Maximum[Axis], Records[Index].Maximum[Axis]);
				}
			auto OutwardMin = [](float Value) { return std::nextafter(Value, -std::numeric_limits<float>::infinity()); };
			auto OutwardMax = [](float Value) { return std::nextafter(Value, std::numeric_limits<float>::infinity()); };
			Candidate.Nodes[NodeIndex].Minimum = {OutwardMin(Minimum[0]), OutwardMin(Minimum[1]), OutwardMin(Minimum[2])};
			Candidate.Nodes[NodeIndex].Maximum = {OutwardMax(Maximum[0]), OutwardMax(Maximum[1]), OutwardMax(Maximum[2])};
			const size_t Count = End - Begin;
			if (Count <= CollisionTrianglesPerLeaf)
			{
				std::sort(Records.begin() + static_cast<ptrdiff_t>(Begin),
					Records.begin() + static_cast<ptrdiff_t>(End),
					[](const auto& Left, const auto& Right) { return Left.Ordinal < Right.Ordinal; });
				Candidate.Nodes[NodeIndex].First = static_cast<uint32>(Candidate.TriangleOrdinals.size());
				Candidate.Nodes[NodeIndex].CountOrSecond = LeafFlag | static_cast<uint32>(Count);
				for (size_t Index = Begin; Index < End; ++Index)
					Candidate.TriangleOrdinals.push_back(Records[Index].Ordinal);
				return NodeIndex;
			}
			std::array<float, 3> Extent{Maximum[0] - Minimum[0], Maximum[1] - Minimum[1], Maximum[2] - Minimum[2]};
			size_t Axis = Extent[1] > Extent[0] ? 1 : 0;
			if (Extent[2] > Extent[Axis]) Axis = 2;
			std::stable_sort(Records.begin() + static_cast<ptrdiff_t>(Begin),
				Records.begin() + static_cast<ptrdiff_t>(End),
				[Axis](const auto& Left, const auto& Right) {
					if (Left.Centroid[Axis] != Right.Centroid[Axis])
						return Left.Centroid[Axis] < Right.Centroid[Axis];
					return Left.Ordinal < Right.Ordinal;
				});
			const size_t Middle = Begin + Count / 2;
			const uint32 Left = BuildRange(Begin, Middle, Depth + 1);
			const uint32 Right = BuildRange(Middle, End, Depth + 1);
			Candidate.Nodes[NodeIndex].First = Left;
			Candidate.Nodes[NodeIndex].CountOrSecond = Right;
			return NodeIndex;
		};

		BuildRange(0, Records.size(), 1);
		Candidate.LogicalRetainedBytes =
			static_cast<uint64>(Fixture.Positions.size()) * sizeof(FVector3f)
			+ static_cast<uint64>(Fixture.Indices.size()) * sizeof(uint32)
			+ static_cast<uint64>(Candidate.Nodes.size()) * sizeof(FCollisionMeshNodePrototype)
			+ static_cast<uint64>(Candidate.TriangleOrdinals.size()) * sizeof(uint32);
		if (Candidate.MaximumDepth > MaximumCollisionTreeDepth
			|| Candidate.LogicalRetainedBytes > MaximumCollisionPayloadBytes) return false;
		OutFacts = std::move(Candidate);
		return true;
	}

	template <typename T>
	auto AppendLittleEndian(std::vector<uint8>& Bytes, T Value) -> void
	{
		using U = std::make_unsigned_t<T>;
		const U Bits = static_cast<U>(Value);
		for (size_t Byte = 0; Byte < sizeof(T); ++Byte)
			Bytes.push_back(static_cast<uint8>(Bits >> (Byte * 8)));
	}

	template <typename T>
	auto WriteLittleEndian(std::vector<uint8>& Bytes, size_t Offset, T Value) -> void
	{
		using U = std::make_unsigned_t<T>;
		const U Bits = static_cast<U>(Value);
		for (size_t Byte = 0; Byte < sizeof(T); ++Byte)
			Bytes[Offset + Byte] = static_cast<uint8>(Bits >> (Byte * 8));
	}

	template <typename T>
	auto ReadLittleEndian(std::span<const uint8> Bytes, size_t Offset) -> T
	{
		using U = std::make_unsigned_t<T>;
		U Bits = 0;
		for (size_t Byte = 0; Byte < sizeof(T); ++Byte)
			Bits |= static_cast<U>(Bytes[Offset + Byte]) << (Byte * 8);
		return static_cast<T>(Bits);
	}

	auto AppendString(std::vector<uint8>& Bytes, std::string_view Value) -> void
	{
		AppendLittleEndian<uint64>(Bytes, Value.size());
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	auto BuildCollisionKeyBytes(
		const FXxHash128& SourceHash,
		const FXxHash128& GeometryHash,
		ECollisionSourceMode Mode,
		ECollisionQueryPolicy Policy,
		uint32 WeldToleranceBits,
		std::string_view Importer,
		uint32 ImporterVersion,
		std::array<uint8, 3> ImportAxes,
		uint32 TargetPlatform) -> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		AppendLittleEndian<uint32>(Bytes, CollisionKeySchemaVersion);
		AppendLittleEndian<uint64>(Bytes, SourceHash.HashLow);
		AppendLittleEndian<uint64>(Bytes, SourceHash.HashHigh);
		AppendLittleEndian<uint64>(Bytes, GeometryHash.HashLow);
		AppendLittleEndian<uint64>(Bytes, GeometryHash.HashHigh);
		AppendString(Bytes, Importer);
		AppendLittleEndian<uint32>(Bytes, ImporterVersion);
		for (uint8 Axis : ImportAxes) AppendLittleEndian<uint8>(Bytes, Axis);
		AppendLittleEndian<uint8>(Bytes, static_cast<uint8>(Mode));
		AppendLittleEndian<uint8>(Bytes, static_cast<uint8>(Policy));
		AppendLittleEndian<uint32>(Bytes, WeldToleranceBits);
		AppendLittleEndian<uint32>(Bytes, CollisionBuilderVersion);
		AppendLittleEndian<uint32>(Bytes, CollisionPayloadSchemaVersion);
		AppendLittleEndian<uint32>(Bytes, TargetPlatform);
		return Bytes;
	}

	auto BuildPrototypePayload(const FCollisionSourceFixture& Fixture) -> std::vector<uint8>
	{
		FMeshBuildFacts Facts;
		if (!BuildMeshPrototype(Fixture, Facts)) return {};
		struct FChunk
		{
			uint32 Type;
			uint64 Count;
			std::vector<uint8> Data;
		};
		std::array<FChunk, 4> Chunks{{
			{1, Fixture.Positions.size(), {}},
			{2, Fixture.Indices.size(), {}},
			{3, Facts.TriangleOrdinals.size(), {}},
			{4, Facts.Nodes.size(), {}}}};
		for (const FVector3f& Position : Fixture.Positions)
		{
			AppendLittleEndian<uint32>(Chunks[0].Data, std::bit_cast<uint32>(Position.x));
			AppendLittleEndian<uint32>(Chunks[0].Data, std::bit_cast<uint32>(Position.y));
			AppendLittleEndian<uint32>(Chunks[0].Data, std::bit_cast<uint32>(Position.z));
		}
		for (uint32 Index : Fixture.Indices) AppendLittleEndian<uint32>(Chunks[1].Data, Index);
		for (uint32 Ordinal : Facts.TriangleOrdinals)
			AppendLittleEndian<uint32>(Chunks[2].Data, Ordinal);
		for (const FCollisionMeshNodePrototype& Node : Facts.Nodes)
		{
			for (float Value : {Node.Minimum.x, Node.Minimum.y, Node.Minimum.z})
				AppendLittleEndian<uint32>(Chunks[3].Data, std::bit_cast<uint32>(Value));
			AppendLittleEndian<uint32>(Chunks[3].Data, Node.First);
			for (float Value : {Node.Maximum.x, Node.Maximum.y, Node.Maximum.z})
				AppendLittleEndian<uint32>(Chunks[3].Data, std::bit_cast<uint32>(Value));
			AppendLittleEndian<uint32>(Chunks[3].Data, Node.CountOrSecond);
		}

		std::vector<uint8> Bytes(CollisionPayloadHeaderSize
			+ Chunks.size() * CollisionPayloadChunkEntrySize, 0);
		WriteLittleEndian<uint32>(Bytes, 0, CollisionPayloadMagic);
		WriteLittleEndian<uint32>(Bytes, 4, CollisionPayloadSchemaVersion);
		WriteLittleEndian<uint32>(Bytes, 8, CollisionBuilderVersion);
		WriteLittleEndian<uint32>(Bytes, 12, 1u); // Win64
		WriteLittleEndian<uint32>(Bytes, 16, CollisionPayloadHeaderSize);
		WriteLittleEndian<uint32>(Bytes, 20, static_cast<uint32>(Chunks.size()));
		WriteLittleEndian<uint32>(Bytes, 24, CollisionPayloadAlignment);
		for (size_t ChunkIndex = 0; ChunkIndex < Chunks.size(); ++ChunkIndex)
		{
			const size_t Aligned = (Bytes.size() + CollisionPayloadAlignment - 1)
				& ~(static_cast<size_t>(CollisionPayloadAlignment) - 1);
			Bytes.resize(Aligned, 0);
			const size_t Entry = CollisionPayloadHeaderSize
				+ ChunkIndex * CollisionPayloadChunkEntrySize;
			WriteLittleEndian<uint32>(Bytes, Entry, Chunks[ChunkIndex].Type);
			WriteLittleEndian<uint32>(Bytes, Entry + 4, 1u); // required
			WriteLittleEndian<uint64>(Bytes, Entry + 8, Aligned);
			WriteLittleEndian<uint64>(Bytes, Entry + 16, Chunks[ChunkIndex].Data.size());
			WriteLittleEndian<uint64>(Bytes, Entry + 24, Chunks[ChunkIndex].Count);
			Bytes.insert(Bytes.end(), Chunks[ChunkIndex].Data.begin(), Chunks[ChunkIndex].Data.end());
		}
		WriteLittleEndian<uint64>(Bytes, 32, Bytes.size());
		WriteLittleEndian<uint64>(Bytes, 40, Facts.LogicalRetainedBytes);
		WriteLittleEndian<uint64>(Bytes, 48,
			FXxHash64::HashBuffer(std::span<const uint8>(Bytes).subspan(64)).HashValue);
		return Bytes;
	}

	auto ValidatePrototypePayload(std::span<const uint8> Bytes) -> bool
	{
		if (Bytes.size() < CollisionPayloadHeaderSize
			|| ReadLittleEndian<uint32>(Bytes, 0) != CollisionPayloadMagic
			|| ReadLittleEndian<uint32>(Bytes, 4) != CollisionPayloadSchemaVersion
			|| ReadLittleEndian<uint32>(Bytes, 12) != 1u
			|| ReadLittleEndian<uint32>(Bytes, 16) != CollisionPayloadHeaderSize
			|| ReadLittleEndian<uint32>(Bytes, 24) != CollisionPayloadAlignment
			|| ReadLittleEndian<uint64>(Bytes, 32) != Bytes.size()
			|| ReadLittleEndian<uint64>(Bytes, 48)
				!= FXxHash64::HashBuffer(Bytes.subspan(64)).HashValue) return false;
		const uint32 ChunkCount = ReadLittleEndian<uint32>(Bytes, 20);
		if (ChunkCount != 4 || ChunkCount > MaximumCollisionPayloadChunks
			|| CollisionPayloadHeaderSize + static_cast<uint64>(ChunkCount)
				* CollisionPayloadChunkEntrySize > Bytes.size()) return false;
		uint64 PreviousEnd = CollisionPayloadHeaderSize
			+ static_cast<uint64>(ChunkCount) * CollisionPayloadChunkEntrySize;
		for (uint32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
		{
			const size_t Entry = CollisionPayloadHeaderSize
				+ ChunkIndex * CollisionPayloadChunkEntrySize;
			const uint32 Type = ReadLittleEndian<uint32>(Bytes, Entry);
			const uint32 Flags = ReadLittleEndian<uint32>(Bytes, Entry + 4);
			const uint64 Offset = ReadLittleEndian<uint64>(Bytes, Entry + 8);
			const uint64 Size = ReadLittleEndian<uint64>(Bytes, Entry + 16);
			const uint64 Count = ReadLittleEndian<uint64>(Bytes, Entry + 24);
			if (Type != ChunkIndex + 1 || Flags != 1u || Count == 0
				|| Offset % CollisionPayloadAlignment != 0 || Offset < PreviousEnd
				|| Size > Bytes.size() - Offset) return false;
			const uint64 ElementSize = Type == 1 ? sizeof(FVector3f)
				: Type == 4 ? sizeof(FCollisionMeshNodePrototype) : sizeof(uint32);
			if (Count > std::numeric_limits<uint64>::max() / ElementSize
				|| Count * ElementSize != Size) return false;
			PreviousEnd = Offset + Size;
		}
		return PreviousEnd == Bytes.size();
	}
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesSourceModesPoliciesAndDefaultCompatibility)
{
	EXPECT_EQ(ResolveRepresentation(ECollisionQueryPolicy::SimpleAndComplex,
		ECollisionQueryComplexity::Default, true, true), ECollisionRepresentation::Simple);
	EXPECT_EQ(ResolveRepresentation(ECollisionQueryPolicy::SimpleAndComplex,
		ECollisionQueryComplexity::Default, false, true), ECollisionRepresentation::Complex);
	EXPECT_EQ(ResolveRepresentation(ECollisionQueryPolicy::SimpleAndComplex,
		ECollisionQueryComplexity::Simple, false, true), ECollisionRepresentation::None);
	EXPECT_EQ(ResolveRepresentation(ECollisionQueryPolicy::SimpleOnly,
		ECollisionQueryComplexity::Complex, true, true), ECollisionRepresentation::None);
	EXPECT_EQ(ResolveRepresentation(ECollisionQueryPolicy::ComplexOnly,
		ECollisionQueryComplexity::Default, true, true), ECollisionRepresentation::Complex);
	EXPECT_EQ(static_cast<uint8>(ECollisionSourceMode::None), 0u);
	EXPECT_EQ(static_cast<uint8>(ECollisionSourceMode::ConvexHullFromLOD0), 1u);
	EXPECT_EQ(static_cast<uint8>(ECollisionSourceMode::TriangleMeshFromLOD0), 2u);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesCompleteTargetAlgorithmMatrix)
{
	for (EPrototypeTarget Target : {EPrototypeTarget::ConvexHull, EPrototypeTarget::TriangleMesh})
		for (EPrototypeOperation Operation : {EPrototypeOperation::Ray, EPrototypeOperation::Sweep,
			EPrototypeOperation::Overlap})
			for (EPrototypeQueryShape Query : {EPrototypeQueryShape::Box,
				EPrototypeQueryShape::Sphere, EPrototypeQueryShape::Capsule})
				EXPECT_LE(static_cast<uint8>(SelectAlgorithm(Operation, Query, Target)),
					static_cast<uint8>(EPrototypeAlgorithm::BoundedFeatureAdvancement));
	EXPECT_EQ(SelectAlgorithm(EPrototypeOperation::Overlap, EPrototypeQueryShape::Box,
		EPrototypeTarget::TriangleMesh), EPrototypeAlgorithm::BoxTriangleSAT);
	EXPECT_EQ(SelectAlgorithm(EPrototypeOperation::Overlap, EPrototypeQueryShape::Capsule,
		EPrototypeTarget::TriangleMesh), EPrototypeAlgorithm::ClosestTriangleFeature);
	EXPECT_EQ(SelectAlgorithm(EPrototypeOperation::Sweep, EPrototypeQueryShape::Sphere,
		EPrototypeTarget::TriangleMesh), EPrototypeAlgorithm::BoundedFeatureAdvancement);
	constexpr uint32 MaximumAdvancementIterations = 32;
	constexpr double PenetrationEpsilon = 1.0e-8;
	constexpr double TimeTolerance = 1.0e-12;
	EXPECT_EQ(MaximumAdvancementIterations, 32u);
	EXPECT_DOUBLE_EQ(PenetrationEpsilon, 1.0e-8);
	EXPECT_DOUBLE_EQ(TimeTolerance, 1.0e-12);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesAnalyticReferenceEvidenceForEveryMatrixCell)
{
	const std::vector<FReferenceEvidence> Evidence = MakeReferenceEvidence();
	ASSERT_EQ(Evidence.size(), 18u);
	for (const FReferenceEvidence& Entry : Evidence)
	{
		EXPECT_TRUE(Entry.bHit);
		EXPECT_TRUE(std::isfinite(Entry.Time));
		EXPECT_GE(Entry.Time, 0.0);
		EXPECT_LE(Entry.Time, 1.0);
		EXPECT_NEAR(Math::Length(Entry.Normal), 1.0, 1.0e-12);
		EXPECT_LE(static_cast<uint8>(SelectAlgorithm(Entry.Operation, Entry.Query, Entry.Target)),
			static_cast<uint8>(EPrototypeAlgorithm::BoundedFeatureAdvancement));
	}
	const auto Find = [&](EPrototypeTarget Target, EPrototypeOperation Operation,
		EPrototypeQueryShape Query) -> const FReferenceEvidence& {
		return *std::ranges::find_if(Evidence, [&](const FReferenceEvidence& Entry) {
			return Entry.Target == Target && Entry.Operation == Operation && Entry.Query == Query;
		});
	};
	EXPECT_NEAR(Find(EPrototypeTarget::TriangleMesh, EPrototypeOperation::Ray,
		EPrototypeQueryShape::Box).Time, 0.5, 1.0e-12);
	EXPECT_NEAR(Find(EPrototypeTarget::ConvexHull, EPrototypeOperation::Sweep,
		EPrototypeQueryShape::Sphere).Time, 1.75 / 6.0, 1.0e-12);
	EXPECT_NEAR(Find(EPrototypeTarget::TriangleMesh, EPrototypeOperation::Sweep,
		EPrototypeQueryShape::Capsule).Time, 0.25, 1.0e-12);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesFixtureCorpusAndDeterministicAssetTree)
{
	const std::vector<FCollisionSourceFixture> Fixtures = MakeEdgeCaseFixtures();
	ASSERT_EQ(Fixtures.size(), 8u);
	EXPECT_EQ(Fixtures[0].Name, "Tetrahedron");
	EXPECT_EQ(Fixtures[1].Indices.size() / 3, 12u);
	EXPECT_EQ(Fixtures[2].Name, "Open");
	EXPECT_EQ(Fixtures[3].Name, "NonManifold");
	EXPECT_EQ(Fixtures[4].Name, "DuplicateAndDegenerate");
	EXPECT_EQ(Fixtures[5].Name, "ThinLargeCoordinate");
	EXPECT_EQ(Fixtures[6].Name, "ReversedWinding");
	EXPECT_EQ(Fixtures[7].Name, "Clustered");

	FMeshBuildFacts First;
	FMeshBuildFacts Second;
	ASSERT_TRUE(BuildMeshPrototype(Fixtures[1], First));
	ASSERT_TRUE(BuildMeshPrototype(Fixtures[1], Second));
	EXPECT_EQ(First.Nodes.size(), 3u);
	EXPECT_EQ(First.TriangleOrdinals.size(), 12u);
	EXPECT_EQ(First.MaximumDepth, 2u);
	EXPECT_EQ(First.LogicalRetainedBytes, 384u);
	EXPECT_EQ(std::memcmp(First.Nodes.data(), Second.Nodes.data(),
		First.Nodes.size() * sizeof(FCollisionMeshNodePrototype)), 0);
	EXPECT_EQ(First.TriangleOrdinals, Second.TriangleOrdinals);
	EXPECT_FALSE(First.Nodes.front().IsLeaf());
	EXPECT_LT(First.Nodes.front().Minimum.x, -1.0f);
	EXPECT_GT(First.Nodes.front().Maximum.x, 1.0f);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesDegeneracyAndTransactionalFailure)
{
	const std::vector<FCollisionSourceFixture> Fixtures = MakeEdgeCaseFixtures();
	FMeshBuildFacts DuplicateFacts;
	ASSERT_TRUE(BuildMeshPrototype(Fixtures[4], DuplicateFacts));
	EXPECT_EQ(DuplicateFacts.SourceTriangles, 3u);
	EXPECT_EQ(DuplicateFacts.RetainedTriangles, 2u);
	EXPECT_EQ(DuplicateFacts.RemovedTriangles, 1u);

	FMeshBuildFacts Sentinel;
	Sentinel.SourceTriangles = 77;
	FCollisionSourceFixture Invalid{"Invalid", {{0, 0, 0}}, {0, 1, 2}};
	EXPECT_FALSE(BuildMeshPrototype(Invalid, Sentinel));
	EXPECT_EQ(Sentinel.SourceTriangles, 77u);
	Invalid = {"NonFinite", {{0, 0, std::numeric_limits<float>::quiet_NaN()},
		{1, 0, 0}, {0, 1, 0}}, {0, 1, 2}};
	EXPECT_FALSE(BuildMeshPrototype(Invalid, Sentinel));
	EXPECT_EQ(Sentinel.SourceTriangles, 77u);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesClosedHullInputAndCanonicalVertexOrder)
{
	const std::vector<FCollisionSourceFixture> Fixtures = MakeEdgeCaseFixtures();
	EXPECT_TRUE(ValidateClosedHullInput(Fixtures[0]));
	EXPECT_TRUE(ValidateClosedHullInput(Fixtures[1]));
	EXPECT_FALSE(ValidateClosedHullInput(Fixtures[2]));
	EXPECT_FALSE(ValidateClosedHullInput(Fixtures[3]));
	EXPECT_FALSE(ValidateClosedHullInput(Fixtures[4]));
	EXPECT_TRUE(ValidateClosedHullInput(Fixtures[6]));

	auto Canonicalize = [](std::vector<FVector3f> Vertices) {
		std::ranges::sort(Vertices, [](const FVector3f& Left, const FVector3f& Right) {
			return std::tuple(Left.x, Left.y, Left.z) < std::tuple(Right.x, Right.y, Right.z);
		});
		Vertices.erase(std::unique(Vertices.begin(), Vertices.end()), Vertices.end());
		return Vertices;
	};
	std::vector<FVector3f> Permuted = Fixtures[1].Positions;
	std::ranges::reverse(Permuted);
	EXPECT_EQ(Canonicalize(Fixtures[1].Positions), Canonicalize(std::move(Permuted)));
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesHullAndMeshMemoryBudgets)
{
	constexpr uint64 CubeHullBytes = 8u * sizeof(FVector3f)
		+ 6u * sizeof(FCollisionHullPlanePrototype)
		+ 24u * sizeof(FCollisionHullHalfEdgePrototype)
		+ 6u * sizeof(FCollisionHullFacePrototype);
	EXPECT_EQ(CubeHullBytes, 672u);
	EXPECT_EQ(MaximumConvexHullVertices, 256u);

	constexpr uint64 WorstCasePositionBytes =
		static_cast<uint64>(MaximumCollisionTriangles) * 3u * sizeof(FVector3f);
	constexpr uint64 WorstCaseIndexBytes =
		static_cast<uint64>(MaximumCollisionTriangles) * 3u * sizeof(uint32);
	constexpr uint64 WorstCaseOrdinalBytes =
		static_cast<uint64>(MaximumCollisionTriangles) * sizeof(uint32);
	constexpr uint64 MaximumLeafCount =
		(MaximumCollisionTriangles + CollisionTrianglesPerLeaf - 1) / CollisionTrianglesPerLeaf;
	constexpr uint64 WorstCaseNodeBytes =
		(MaximumLeafCount * 2u - 1u) * sizeof(FCollisionMeshNodePrototype);
	constexpr uint64 WorstCaseRuntimeBytes = WorstCasePositionBytes + WorstCaseIndexBytes
		+ WorstCaseOrdinalBytes + WorstCaseNodeBytes;
	EXPECT_EQ(WorstCaseRuntimeBytes, 119'999'968u);
	EXPECT_LT(WorstCaseRuntimeBytes, MaximumCollisionPayloadBytes);
	EXPECT_LT(WorstCaseRuntimeBytes * 2u, MaximumCollisionBuilderPeakBytes);
	EXPECT_EQ(CollisionTraversalStackEntries, MaximumCollisionTreeDepth * 2u);
	EXPECT_EQ(MaximumCollisionDebugTriangles, 256u);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesKeyFormatPayloadAndDescriptorIdentity)
{
	const FXxHash128 Source{0x0123456789abcdefull, 0xfedcba9876543210ull};
	const FXxHash128 Geometry{0x1111222233334444ull, 0xaaaabbbbccccddddull};
	const auto BuildKey = [&](FXxHash128 InSource, FXxHash128 InGeometry,
		ECollisionSourceMode Mode, ECollisionQueryPolicy Policy, uint32 WeldBits,
		std::string_view Importer, uint32 ImporterVersion, std::array<uint8, 3> Axes,
		uint32 Platform) {
		return BuildCollisionKeyBytes(InSource, InGeometry, Mode, Policy, WeldBits,
			Importer, ImporterVersion, Axes, Platform);
	};
	const std::vector<uint8> First = BuildKey(Source, Geometry,
		ECollisionSourceMode::TriangleMeshFromLOD0, ECollisionQueryPolicy::SimpleAndComplex,
		std::bit_cast<uint32>(1.0e-5f), "Assimp", 602, {5, 0, 2}, 1);
	const std::vector<uint8> Second = BuildKey(Source, Geometry,
		ECollisionSourceMode::TriangleMeshFromLOD0, ECollisionQueryPolicy::SimpleAndComplex,
		std::bit_cast<uint32>(1.0e-5f), "Assimp", 602, {5, 0, 2}, 1);
	EXPECT_EQ(First, Second);
	EXPECT_EQ(First.size(), 75u);
	EXPECT_EQ(FXxHash128::HashBuffer(First).ToString(), "31049dc20de3b54a742c931cb587ce92");
	const std::string BaselineHash = FXxHash128::HashBuffer(First).ToString();
	auto ExpectKeyChanged = [&](auto&& Bytes) {
		EXPECT_NE(FXxHash128::HashBuffer(Bytes).ToString(), BaselineHash);
	};
	FXxHash128 ChangedSource = Source;
	++ChangedSource.HashLow;
	ExpectKeyChanged(BuildKey(ChangedSource, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 0, 2}, 1));
	FXxHash128 ChangedGeometry = Geometry;
	++ChangedGeometry.HashHigh;
	ExpectKeyChanged(BuildKey(Source, ChangedGeometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 0, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::ConvexHullFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 0, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::ComplexOnly, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 0, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(2.0e-5f),
		"Assimp", 602, {5, 0, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Other", 603, {5, 0, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 1, 2}, 1));
	ExpectKeyChanged(BuildKey(Source, Geometry, ECollisionSourceMode::TriangleMeshFromLOD0,
		ECollisionQueryPolicy::SimpleAndComplex, std::bit_cast<uint32>(1.0e-5f),
		"Assimp", 602, {5, 0, 2}, 2));

	const std::vector<uint8> Payload = BuildPrototypePayload(MakeTetrahedron());
	ASSERT_FALSE(Payload.empty());
	EXPECT_TRUE(ValidatePrototypePayload(Payload));
	EXPECT_EQ(Payload.size(), 336u);
	EXPECT_EQ(FXxHash128::HashBuffer(Payload).ToString(), "e18caaa3799e0c65edea7a0af09edbf1");
	auto ExpectCorrupt = [&](size_t Offset) {
		std::vector<uint8> Corrupt = Payload;
		Corrupt[Offset] ^= 0x40;
		EXPECT_FALSE(ValidatePrototypePayload(Corrupt));
	};
	ExpectCorrupt(0); // magic
	ExpectCorrupt(4); // schema
	ExpectCorrupt(12); // platform
	ExpectCorrupt(20); // chunk count
	ExpectCorrupt(24); // alignment
	ExpectCorrupt(32); // declared size
	ExpectCorrupt(48); // checksum
	ExpectCorrupt(64 + 8); // first range
	ExpectCorrupt(Payload.size() - 1); // data checksum
	EXPECT_EQ(CollisionPayloadHeaderSize, 64u);
	EXPECT_EQ(CollisionPayloadChunkEntrySize, 32u);
	EXPECT_EQ(CollisionPayloadAlignment, 16u);
	EXPECT_GE(MaximumCollisionPayloadChunks, 6u);
	EXPECT_NE(StaticMeshCollisionPayloadId, StaticMeshPrimaryCookedPayloadId);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, CapturesRealImportedSourceWithoutRetainingRenderPointers)
{
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	std::string Error;
	DStaticMesh* Mesh = Asset::Import::Standard::CreateTransientStaticMeshFromFile(
		Source.generic_string(), nullptr, "M3CollisionSourceFixture", Error);
	ASSERT_NE(Mesh, nullptr) << Error;
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_FALSE(RenderData->LODResources.empty());
	const FStaticMeshLODResources& LOD = RenderData->LODResources.front();
	FCollisionSourceFixture Snapshot{
		"ImportedMultiSection",
		LOD.VertexBuffers.PositionVertexBuffer.GetPositions(),
		LOD.IndexBuffer.GetIndices()};
	ASSERT_FALSE(Snapshot.Positions.empty());
	ASSERT_FALSE(Snapshot.Indices.empty());
	FMeshBuildFacts Facts;
	EXPECT_TRUE(BuildMeshPrototype(Snapshot, Facts));
	EXPECT_EQ(Facts.SourceTriangles, Snapshot.Indices.size() / 3);
	EXPECT_LT(Facts.LogicalRetainedBytes, MaximumCollisionPayloadBytes);
	MarkObjectHierarchyAsGarbage(Mesh);
	CollectGarbage();
	EXPECT_FALSE(Snapshot.Positions.empty());
	EXPECT_FALSE(Snapshot.Indices.empty());
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage0Tests, FreezesFeatureOrderingNormalsAndInspectionFacts)
{
	struct FFeatureCandidate { double Time; uint32 Ordinal; };
	std::array Candidates{FFeatureCandidate{0.25, 7}, FFeatureCandidate{0.25, 3},
		FFeatureCandidate{0.5, 1}};
	std::ranges::sort(Candidates, {}, [](const FFeatureCandidate& Value) {
		return std::pair(Value.Time, Value.Ordinal);
	});
	EXPECT_EQ(Candidates.front().Ordinal, 3u);

	auto OrientNormal = [](FVector3 Normal, const FVector3& Motion) {
		return Math::Dot(Normal, Motion) > 0.0 ? -Normal : Normal;
	};
	EXPECT_EQ(OrientNormal({0, 0, 1}, {0, 0, -1}), FVector3(0, 0, 1));
	EXPECT_EQ(OrientNormal({0, 0, -1}, {0, 0, -1}), FVector3(0, 0, 1));
	EXPECT_FALSE(Math::Dot(FVector3(1, 0, 0), FVector3(0, 1, 0)) < 0.0); // tangent does not enter
	EXPECT_TRUE(Math::Dot(FVector3(-1, 0, 0), FVector3(1, 0, 0)) < 0.0);
	EXPECT_TRUE(IsAcceptedPhysicsScale({2.0, 1.0, 0.5}));
	EXPECT_FALSE(IsAcceptedPhysicsScale({-1.0, 1.0, 1.0}));
	EXPECT_FALSE(IsAcceptedPhysicsScale({0.0, 1.0, 1.0}));
	EXPECT_FALSE(IsAcceptedPhysicsScale({std::numeric_limits<double>::quiet_NaN(), 1.0, 1.0}));

	constexpr std::array<std::string_view, 12> InspectorFields{
		"Mode", "Policy", "SourceTriangles", "RetainedTriangles", "RemovedTriangles",
		"Bounds", "PayloadBytes", "RuntimeBytes", "BuilderVersion", "SchemaVersion",
		"CacheCookStatus", "RevisionCoherence"};
	EXPECT_EQ(InspectorFields.size(), 12u);
	EXPECT_EQ(MaximumCollisionDebugTriangles, 256u);
	constexpr uint64 QualifiedSharedInstances = 10'000;
	EXPECT_EQ(QualifiedSharedInstances, 10'000u);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage3Tests, ProductionKeyAndPayloadMatchFrozenGoldenBytes)
{
	const Asset::Build::FStaticMeshCollisionBuildKeyInput KeyInput{
		.SourceContentHash = {0x0123456789abcdefull, 0xfedcba9876543210ull},
		.GeometryHash = {0x1111222233334444ull, 0xaaaabbbbccccddddull},
		.ImporterId = "Assimp",
		.ImporterVersion = 602,
		.ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward(),
		.SourceMode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		.QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		.WeldToleranceBits = std::bit_cast<uint32>(1.0e-5f),
		.TargetPlatform = EStaticMeshTargetPlatform::Win64};
	std::string Error;
	const std::vector<uint8> KeyBytes =
		Asset::Build::BuildStaticMeshCollisionDerivedDataKeyBytes(KeyInput, Error);
	ASSERT_TRUE(Error.empty()) << Error;
	EXPECT_EQ(KeyBytes.size(), 75u);
	EXPECT_EQ(FXxHash128::HashBuffer(KeyBytes).ToString(), "31049dc20de3b54a742c931cb587ce92");
	EXPECT_EQ(Asset::Build::BuildStaticMeshCollisionDerivedDataKey(KeyInput, Error),
		"31049dc20de3b54a742c931cb587ce92");

	const FCollisionSourceFixture Tetra = MakeTetrahedron();
	std::vector<FVector3> Positions;
	for (const FVector3f& Position : Tetra.Positions) Positions.emplace_back(Position);
	const FCollisionGeometryRef Geometry = FCollisionGeometryRef::BuildTriangleMesh(
		Positions, Tetra.Indices);
	ASSERT_TRUE(Geometry.IsValid());
	FStaticMeshCollisionPayloadData Payload;
	ASSERT_TRUE(MakeStaticMeshCollisionPayloadData(Geometry,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Payload, Error)) << Error;
	std::vector<uint8> First;
	std::vector<uint8> Second;
	ASSERT_TRUE(EncodeCollisionPayload(
		Payload, EStaticMeshTargetPlatform::Win64, First, Error)) << Error;
	ASSERT_TRUE(EncodeCollisionPayload(
		Payload, EStaticMeshTargetPlatform::Win64, Second, Error)) << Error;
	EXPECT_EQ(First, Second);
	EXPECT_EQ(First.size(), 336u);
	EXPECT_EQ(FXxHash128::HashBuffer(First).ToString(), "e18caaa3799e0c65edea7a0af09edbf1");
	FStaticMeshCollisionPayloadData Decoded;
	ASSERT_TRUE(DecodeCollisionPayload(
		First, EStaticMeshTargetPlatform::Win64, Decoded));
	FCollisionGeometryRef RoundTrip;
	ASSERT_TRUE(MakeStaticMeshCollisionGeometry(Decoded, RoundTrip, Error)) << Error;
	EXPECT_EQ(RoundTrip.GetTriangleCount(), Geometry.GetTriangleCount());
	EXPECT_EQ(RoundTrip.GetNodeCount(), Geometry.GetNodeCount());
	std::vector<uint8> Corrupt = First;
	Corrupt.back() ^= 0x80;
	FStaticMeshCollisionPayloadData Preserved = Decoded;
	EXPECT_FALSE(DecodeCollisionPayload(
		Corrupt, EStaticMeshTargetPlatform::Win64, Preserved));
	EXPECT_EQ(Preserved.Indices, Decoded.Indices);
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage5Tests, InspectionReportsBoundedReadOnlyCollisionFacts)
{
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	std::string Error;
	DStaticMesh* Mesh = Asset::Import::Standard::CreateTransientStaticMeshFromFile(
		Source.generic_string(), nullptr, "M3CollisionInspectionFixture", Error);
	ASSERT_NE(Mesh, nullptr) << Error;
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	const FStaticMeshCollisionInspection Inspection = Mesh->InspectCollision();
	EXPECT_EQ(Inspection.Mode, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Inspection.Policy, EBodySetupCollisionQueryPolicy::SimpleAndComplex);
	EXPECT_TRUE(Inspection.bHasGeometry);
	EXPECT_EQ(Inspection.GeometryKind, ECollisionGeometryKind::TriangleMesh);
	EXPECT_GT(Inspection.SourceTriangles, 0u);
	EXPECT_GT(Inspection.RetainedTriangles, 0u);
	EXPECT_EQ(Inspection.SourceTriangles,
		Inspection.RetainedTriangles + Inspection.RemovedTriangles);
	EXPECT_GT(Inspection.Nodes, 0u);
	EXPECT_TRUE(Inspection.Bounds.has_value());
	EXPECT_GT(Inspection.PayloadBytes, 0u);
	EXPECT_GT(Inspection.RuntimeBytes, 0u);
	EXPECT_EQ(Inspection.BuilderVersion, StaticMeshCollisionBuilderVersion);
	EXPECT_EQ(Inspection.SchemaVersion, StaticMeshCollisionPayloadSchemaVersion);
	EXPECT_GT(Inspection.BuildRevision, 0u);
	EXPECT_TRUE(Inspection.bRevisionCoherent);
	EXPECT_FALSE(Inspection.CacheKey.empty());
	EXPECT_FALSE(Inspection.Diagnostic.empty());

	const FStaticMeshCollisionInspection Frozen = Inspection;
	EXPECT_EQ(Frozen.RetainedTriangles, Inspection.RetainedTriangles);
	EXPECT_EQ(Frozen.RuntimeBytes, Inspection.RuntimeBytes);
	MarkObjectHierarchyAsGarbage(Mesh);
	CollectGarbage();
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage3Tests, StaticMeshAuthorshipUsesIndependentDdcAndTransactionalFailure)
{
	const std::string PreviousCache = FPaths::DerivedDataCacheDir();
	const std::filesystem::path Cache = Testing::GetTestWorkDirectory() / "CollisionDDC";
	Testing::RemoveTestWorkDirectory(Cache);
	FPaths::SetDerivedDataCacheDirForTests(Cache.generic_string());
	DStaticMesh* Mesh = DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);
	std::string Error;
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	DBodySetup* Setup = Mesh->GetBodySetup();
	ASSERT_NE(Setup, nullptr);
	EXPECT_EQ(Setup->GetCollisionSourceMode(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Setup->GetCollisionBuildStatus(), EBodySetupCollisionBuildStatus::Rebuilt);
	const std::string FirstKey = Setup->GetCollisionDerivedDataKey();
	EXPECT_EQ(FirstKey.size(), 32u);
	FCollisionGeometryRef FirstGeometry;
	ASSERT_TRUE(Setup->BuildComplexGeometry(FirstGeometry));
	const uint64 FirstIdentity = FirstGeometry.GetIdentity();
	ASSERT_TRUE(Mesh->RebuildCollision(Error)) << Error;
	EXPECT_EQ(Setup->GetCollisionBuildStatus(), EBodySetupCollisionBuildStatus::CacheHit);
	EXPECT_EQ(Setup->GetCollisionDerivedDataKey(), FirstKey);
	FCollisionGeometryRef CachedGeometry;
	ASSERT_TRUE(Setup->BuildComplexGeometry(CachedGeometry));
	EXPECT_NE(CachedGeometry.GetIdentity(), FirstIdentity);
	EXPECT_EQ(CachedGeometry.GetTriangleCount(), FirstGeometry.GetTriangleCount());

	EXPECT_FALSE(Mesh->SetCollisionSourceMode(
		EBodySetupCollisionSourceMode::ConvexHullFromLOD0, Error));
	EXPECT_EQ(Setup->GetCollisionSourceMode(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	FCollisionGeometryRef Preserved;
	ASSERT_TRUE(Setup->BuildComplexGeometry(Preserved));
	EXPECT_EQ(Preserved.GetIdentity(), CachedGeometry.GetIdentity());
	ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(
		EBodySetupCollisionQueryPolicy::ComplexOnly, Error)) << Error;
	EXPECT_EQ(Setup->GetCollisionQueryPolicy(), EBodySetupCollisionQueryPolicy::ComplexOnly);
	EXPECT_NE(Setup->GetCollisionDerivedDataKey(), FirstKey);
	EXPECT_EQ(Setup->GetCollisionBuildStatus(), EBodySetupCollisionBuildStatus::Rebuilt);
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None, Error));
	EXPECT_FALSE(Setup->BuildComplexGeometry(Preserved));
	FPaths::SetDerivedDataCacheDirForTests(PreviousCache);
	MarkObjectHierarchyAsGarbage(Mesh);
	CollectGarbage();
}

DURIN_STATIC_MESH_COLLISION_ROUTINE_TEST(FAetherCookedCollisionStage3Tests, ImportedStateExchangeMovesCollisionAsOneReversibleBundle)
{
	const std::string PreviousCache = FPaths::DerivedDataCacheDir();
	const std::filesystem::path Cache = Testing::GetTestWorkDirectory() / "CollisionExchangeDDC";
	Testing::RemoveTestWorkDirectory(Cache);
	FPaths::SetDerivedDataCacheDirForTests(Cache.generic_string());
	DStaticMesh* Target = DStaticMesh::CreateDebugTriangle();
	DStaticMesh* Candidate = DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Target, nullptr);
	ASSERT_NE(Candidate, nullptr);
	std::string Error;
	ASSERT_TRUE(Target->SetCollisionSourceMode(
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	ASSERT_TRUE(Candidate->SetCollisionSourceMode(
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	ASSERT_TRUE(Candidate->SetCollisionQueryPolicy(
		EBodySetupCollisionQueryPolicy::ComplexOnly, Error)) << Error;
	const uint64 TargetRevision = Target->GetBodySetup()->GetCollisionBuildRevision();
	const uint64 CandidateRevision = Candidate->GetBodySetup()->GetCollisionBuildRevision();
	const std::string TargetKey = Target->GetBodySetup()->GetCollisionDerivedDataKey();
	const std::string CandidateKey = Candidate->GetBodySetup()->GetCollisionDerivedDataKey();
	auto Exchange = Target->PrepareImportedStateExchange(*Candidate, Error);
	ASSERT_NE(Exchange, nullptr) << Error;
	Exchange->Commit();
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionQueryPolicy(),
		EBodySetupCollisionQueryPolicy::ComplexOnly);
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionDerivedDataKey(), CandidateKey);
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionBuildRevision(), CandidateRevision);
	Exchange->Reverse();
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionQueryPolicy(),
		EBodySetupCollisionQueryPolicy::SimpleAndComplex);
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionDerivedDataKey(), TargetKey);
	EXPECT_EQ(Target->GetBodySetup()->GetCollisionBuildRevision(), TargetRevision);
	Exchange->Finalize();
	FPaths::SetDerivedDataCacheDirForTests(PreviousCache);
	MarkObjectHierarchyAsGarbage(Target);
	MarkObjectHierarchyAsGarbage(Candidate);
	CollectGarbage();
}
