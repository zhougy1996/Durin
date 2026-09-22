#include "Physics/PhysicsDerivedData.h"
#include "Asset/PayloadBuildControl.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	using AssetPrivate::FPayloadBuildControl;
	using AssetPrivate::FPayloadBuildCancelled;
	namespace
	{
		auto ResolvePhysicsTarget(FArchive& Ar, EAssetPayloadTargetPlatform& Platform) -> bool
		{
			if (Ar.GetTarget().Platform != "Win64")
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Physics Archive target is missing or unsupported.");
				return false;
			}
			Platform = EAssetPayloadTargetPlatform::Win64;
			return true;
		}
	}
	namespace
	{
		auto AlignCollisionOffset(uint64 Offset) -> uint64
		{
			return (Offset + PhysicsCollisionPayloadAlignment - 1)
				& ~(static_cast<uint64>(PhysicsCollisionPayloadAlignment) - 1);
		}

	}

	auto FormatPhysicsCacheCodecError(const FPhysicsCacheCodecError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPhysicsCacheCodecError::None: return {};
		case EPhysicsCacheCodecError::CollisionPayload:
			return Error.CollisionCause ? FormatPhysicsCollisionPayloadError(*Error.CollisionCause) : "Physics payload conversion failed.";
		case EPhysicsCacheCodecError::Archive:
			return std::format("Physics cache operation {} failed at byte {} (Archive code {}, path '{}').",
				static_cast<int>(Error.Operation), Error.Actual, Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		case EPhysicsCacheCodecError::CollisionMetadata:
			return std::format("Cached physics mode/policy ({}/{}) does not match ({}/{}).",
				static_cast<int>(Error.ActualMode), static_cast<int>(Error.ActualPolicy),
				static_cast<int>(Error.ExpectedMode), static_cast<int>(Error.ExpectedPolicy));
		}
		return {};
	}

	auto FormatPhysicsCollisionPayloadError(const FPhysicsCollisionPayloadError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPhysicsCollisionPayloadError::None: return {};
		case EPhysicsCollisionPayloadError::InvalidGeometry: return "Collision payload requires one valid hull or triangle mesh.";
		case EPhysicsCollisionPayloadError::InvalidVertex: return "Collision geometry contains an invalid vertex.";
		case EPhysicsCollisionPayloadError::FloatStorage: return "Collision vertex is outside finite float32 storage.";
		case EPhysicsCollisionPayloadError::InvalidTriangle: return "Collision geometry has an invalid triangle.";
		case EPhysicsCollisionPayloadError::InvalidNode: return "Collision geometry has an invalid BVH node.";
		case EPhysicsCollisionPayloadError::InvalidMembership: return "Collision geometry has an invalid BVH membership.";
		case EPhysicsCollisionPayloadError::InconsistentCounts: return "Collision payload counts are inconsistent.";
		case EPhysicsCollisionPayloadError::NonFinitePosition: return "Collision payload contains a non-finite position.";
		case EPhysicsCollisionPayloadError::UnexpectedBvh: return "Convex collision payload must not contain a BVH.";
		case EPhysicsCollisionPayloadError::DuplicateOrdinal: return "Collision payload source ordinals are not unique.";
		case EPhysicsCollisionPayloadError::UnknownOrdinal: return "Collision BVH references an unknown source ordinal.";
		case EPhysicsCollisionPayloadError::InvalidSourceMode: return "Collision payload source mode is invalid.";
		case EPhysicsCollisionPayloadError::InvalidTopology: return "Collision payload topology or BVH is invalid.";
		case EPhysicsCollisionPayloadError::Cancelled: return "Physics payload operation was cancelled.";
		}
		return "Physics collision payload is invalid.";
	}

	auto MakePhysicsCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FPhysicsCollisionPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel) -> std::expected<void, FPhysicsCollisionPayloadError>
	try
	{
		auto Reject = [&](EPhysicsCollisionPayloadError Code, uint64 Index = 0, FVector3 Position = FVector3(0)) {
			std::expected<void, FPhysicsCollisionPayloadError> Result = std::unexpected(FPhysicsCollisionPayloadError{.Code = Code,
				.Operation = EPhysicsCollisionPayloadOperation::Extract,
				.VertexCount = Geometry ? Geometry.GetVertexCount() : 0,
				.IndexCount = Geometry ? uint64(Geometry.GetTriangleCount()) * 3 : 0,
				.NodeCount = Geometry ? Geometry.GetNodeCount() : 0,
				.LeafCount = Geometry ? Geometry.GetLeafTriangleCount() : 0,
				.Index = Index, .Position = Position});
			if (Geometry) Result.error().GeometryKind = Geometry.GetKind();
			return Result;
		};
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (!Geometry || (Geometry.GetKind() != ECollisionGeometryKind::ConvexHull
			&& Geometry.GetKind() != ECollisionGeometryKind::TriangleMesh))
			return Reject(EPhysicsCollisionPayloadError::InvalidGeometry);
		FPhysicsCollisionPayloadData Candidate;
		Candidate.SourceMode = Geometry.GetKind() == ECollisionGeometryKind::ConvexHull
			? EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			: EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		Candidate.QueryPolicy = QueryPolicy;
		Candidate.Positions.reserve(Geometry.GetVertexCount());
		for (uint32 Index = 0; Index < Geometry.GetVertexCount(); ++Index)
		{
			Control.Tick();
			const FVector3* Vertex = Geometry.GetVertex(Index);
			if (!Vertex || !Math::IsFinite(*Vertex))
				return Reject(EPhysicsCollisionPayloadError::InvalidVertex, Index, Vertex ? *Vertex : FVector3(0));
			const FVector3f Stored(*Vertex);
			if (!Math::IsFinite(Stored))
				return Reject(EPhysicsCollisionPayloadError::FloatStorage, Index, *Vertex);
			Candidate.Positions.push_back(Stored);
		}
		Candidate.Indices.reserve(Geometry.GetTriangleCount() * 3);
		Candidate.SourceOrdinals.reserve(Geometry.GetTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetTriangleCount(); ++Index)
		{
			Control.Tick();
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(Index);
			if (!Triangle) return Reject(EPhysicsCollisionPayloadError::InvalidTriangle, Index);
			Candidate.Indices.insert(Candidate.Indices.end(),
				{Triangle->First, Triangle->Second, Triangle->Third});
			Candidate.SourceOrdinals.push_back(Triangle->SourceOrdinal);
		}
		Candidate.Nodes.reserve(Geometry.GetNodeCount());
		for (uint32 Index = 0; Index < Geometry.GetNodeCount(); ++Index)
		{
			Control.Tick();
			const FCollisionGeometryNode* Node = Geometry.GetNode(Index);
			if (!Node) return Reject(EPhysicsCollisionPayloadError::InvalidNode, Index);
			Candidate.Nodes.push_back(*Node);
		}
		Candidate.LeafTriangles.reserve(Geometry.GetLeafTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetLeafTriangleCount(); ++Index)
		{
			Control.Tick();
			const uint32 TriangleIndex = Geometry.GetLeafTriangle(Index);
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(TriangleIndex);
			if (!Triangle) return Reject(EPhysicsCollisionPayloadError::InvalidMembership, Index);
			Candidate.LeafTriangles.push_back(Triangle->SourceOrdinal);
		}
		Control.Check();
		OutPayload = std::move(Candidate);
		return {};
	}
	catch (const FPayloadBuildCancelled&)
	{
		return std::unexpected(FPhysicsCollisionPayloadError{.Code = EPhysicsCollisionPayloadError::Cancelled, .Operation = EPhysicsCollisionPayloadOperation::Extract});
	}

	auto MakePhysicsCollisionGeometry(
		const FPhysicsCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		const std::function<bool()>& ShouldCancel) -> std::expected<void, FPhysicsCollisionPayloadError>
	try
	{
		auto Reject = [&](EPhysicsCollisionPayloadError Code, uint64 Index = 0, uint32 Ordinal = 0,
			FVector3 Position = FVector3(0)) {
			return std::unexpected(FPhysicsCollisionPayloadError{.Code = Code,
				.Operation = EPhysicsCollisionPayloadOperation::Construct, .SourceMode = Payload.SourceMode,
				.VertexCount = Payload.Positions.size(), .IndexCount = Payload.Indices.size(),
				.OrdinalCount = Payload.SourceOrdinals.size(), .NodeCount = Payload.Nodes.size(),
				.LeafCount = Payload.LeafTriangles.size(), .Index = Index, .Ordinal = Ordinal, .Position = Position});
		};
		bool bCancelled = false;
		const std::function<bool()> Cancelled = [&] {
			bCancelled = bCancelled || (ShouldCancel && ShouldCancel());
			return bCancelled;
		};
		FPayloadBuildControl Control{Cancelled};
		Control.Check();
		if (Payload.Positions.empty() || Payload.Indices.empty()
			|| Payload.Indices.size() % 3 != 0
			|| Payload.SourceOrdinals.size() != Payload.Indices.size() / 3)
			return Reject(EPhysicsCollisionPayloadError::InconsistentCounts);
		std::vector<FVector3> Vertices;
		Vertices.reserve(Payload.Positions.size());
		for (const FVector3f& Position : Payload.Positions)
		{
			Control.Tick();
			if (!Math::IsFinite(Position)) return Reject(EPhysicsCollisionPayloadError::NonFinitePosition, static_cast<uint64>(&Position - Payload.Positions.data()), 0, FVector3(Position));
			Vertices.emplace_back(Position);
		}
		FCollisionGeometryRef Candidate;
		if (Payload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
		{
			if (!Payload.Nodes.empty() || !Payload.LeafTriangles.empty())
				return Reject(EPhysicsCollisionPayloadError::UnexpectedBvh);
			Candidate = FCollisionGeometryRef::MakeConvexHull(Vertices, Payload.Indices, Cancelled);
		}
		else if (Payload.SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			std::map<uint32, uint32> OrdinalToTriangle;
			for (uint32 Triangle = 0; Triangle < Payload.SourceOrdinals.size(); ++Triangle)
			{
				Control.Tick();
				if (!OrdinalToTriangle.emplace(Payload.SourceOrdinals[Triangle], Triangle).second)
					return Reject(EPhysicsCollisionPayloadError::DuplicateOrdinal, Triangle, Payload.SourceOrdinals[Triangle]);
			}
			std::vector<uint32> LeafTriangles;
			LeafTriangles.reserve(Payload.LeafTriangles.size());
			for (size_t LeafIndex = 0; LeafIndex < Payload.LeafTriangles.size(); ++LeafIndex)
			{
				const uint32 Ordinal = Payload.LeafTriangles[LeafIndex];
				Control.Tick();
				const auto Found = OrdinalToTriangle.find(Ordinal);
				if (Found == OrdinalToTriangle.end())
					return Reject(EPhysicsCollisionPayloadError::UnknownOrdinal, LeafIndex, Ordinal);
				LeafTriangles.push_back(Found->second);
			}
			Candidate = FCollisionGeometryRef::MakeCookedTriangleMesh(
				Vertices, Payload.Indices, Payload.SourceOrdinals, Payload.Nodes, LeafTriangles, Cancelled);
		}
		else return Reject(EPhysicsCollisionPayloadError::InvalidSourceMode);
		if (!Candidate) return Reject(bCancelled ? EPhysicsCollisionPayloadError::Cancelled
			: EPhysicsCollisionPayloadError::InvalidTopology);
		Control.Check();
		OutGeometry = Candidate;
		return {};
	}
	catch (const FPayloadBuildCancelled&)
	{
		return std::unexpected(FPhysicsCollisionPayloadError{.Code = EPhysicsCollisionPayloadError::Cancelled, .Operation = EPhysicsCollisionPayloadOperation::Construct});
	}

	auto FPhysicsCollisionPayloadData::Serialize(FArchive& Ar,
		const std::function<bool()>& ShouldCancel) -> void
	try
	{
		if (Ar.IsError()) return;
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		auto Reject = [&](EArchiveFailureCode Code, std::string_view Message) { Ar.Fail(Code, Message); };
		EAssetPayloadTargetPlatform TargetPlatform;
		if (!ResolvePhysicsTarget(Ar, TargetPlatform)) return;

		uint32 Reserved0 = 0, Schema = PhysicsCollisionPayloadSchemaVersion;
		uint32 Builder = PhysicsCookBuilderVersion, Platform = static_cast<uint32>(TargetPlatform);
		uint32 Header = PhysicsCollisionPayloadHeaderSize, ChunkCount = 4;
		uint32 Alignment = PhysicsCollisionPayloadAlignment, Mode = 0, Policy = 0, Reserved = 0;
		uint64 StoredSize = 0, LogicalBytes = 0, Checksum = 0;
		const std::array<uint64, 4> ElementSizes{12, 4, 4, 32};
		std::array<uint64, 4> Counts{}, Offsets{}, Sizes{};
		std::array<FByteBuffer, 4> Buffers;
		std::array<std::unique_ptr<FArchive>, 4> Streams;
		std::vector<uint32> OrderedIndices, OrderedOrdinals;
		auto* WireIndices = &Indices;
		auto* WireOrdinals = &SourceOrdinals;
		FByteBuffer Body;
		std::string Error;

		auto TransferStreams = [&] {
			for (auto& Position : Positions)
				for (uint32 Axis = 0; Axis < 3; ++Axis) { Control.Tick(); *Streams[0] << Position[Axis]; }
			for (uint32& Index : *WireIndices) { Control.Tick(); *Streams[1] << Index; }
			for (uint32& Ordinal : *WireOrdinals) { Control.Tick(); *Streams[2] << Ordinal; }
			for (auto& Node : Nodes)
			{
				Control.Tick();
				for (uint32 Axis = 0; Axis < 3; ++Axis) *Streams[3] << Node.Minimum[Axis];
				*Streams[3] << Node.First;
				for (uint32 Axis = 0; Axis < 3; ++Axis) *Streams[3] << Node.Maximum[Axis];
				*Streams[3] << Node.CountOrSecond;
			}
		};
		auto TransferTable = [&](FArchive& Table) {
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				uint32 Type = Chunk + 1, Flags = 1;
				Table << Type << Flags << Offsets[Chunk] << Sizes[Chunk] << Counts[Chunk];
				if (Type != Chunk + 1 || Flags != 1)
					Table.Fail(EArchiveFailureCode::InvalidData, "DCOL chunk identity is invalid.");
			}
		};
		if (Ar.IsSaving())
		{
			// Bound all serialization scratch before geometry validation or ordering.
			Counts = {Positions.size(), Indices.size(), SourceOrdinals.size(), Nodes.size()};
			StoredSize = 64 + 4 * PhysicsCollisionPayloadChunkEntrySize;
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				if (Counts[Chunk] > MaximumPhysicsCollisionPayloadBytes / ElementSizes[Chunk])
					return Reject(EArchiveFailureCode::LimitExceeded, "DCOL count exceeds its byte limit.");
				Sizes[Chunk] = Counts[Chunk] * ElementSizes[Chunk];
				Offsets[Chunk] = AlignCollisionOffset(StoredSize);
				if (Offsets[Chunk] > MaximumPhysicsCollisionPayloadBytes
					|| Sizes[Chunk] > MaximumPhysicsCollisionPayloadBytes - Offsets[Chunk])
					return Reject(EArchiveFailureCode::LimitExceeded, "DCOL payload exceeds its byte limit.");
				StoredSize = Offsets[Chunk] + Sizes[Chunk];
				LogicalBytes += Sizes[Chunk];
			}
			FCollisionGeometryRef Validation;
			if (const auto Built = MakePhysicsCollisionGeometry(*this, Validation, ShouldCancel); !Built)
				return Reject(EArchiveFailureCode::InvalidData, FormatPhysicsCollisionPayloadError(Built.error()));
			if (SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			{
				std::map<uint32, uint32> OrdinalToTriangle;
				for (uint32 Triangle = 0; Triangle < SourceOrdinals.size(); ++Triangle)
					{ Control.Tick(); OrdinalToTriangle.emplace(SourceOrdinals[Triangle], Triangle); }
				OrderedIndices.reserve(Indices.size());
				OrderedOrdinals.reserve(LeafTriangles.size());
				for (uint32 Ordinal : LeafTriangles)
				{
					Control.Tick();
					const auto Found = OrdinalToTriangle.find(Ordinal);
					if (Found == OrdinalToTriangle.end())
						return Reject(EArchiveFailureCode::InvalidData, "DCOL leaf references an unknown source ordinal.");
					const size_t Begin = static_cast<size_t>(Found->second) * 3;
					OrderedIndices.insert(OrderedIndices.end(), Indices.begin() + Begin, Indices.begin() + Begin + 3);
					OrderedOrdinals.push_back(Ordinal);
				}
				if (OrderedIndices.size() != Indices.size() || OrderedOrdinals.size() != SourceOrdinals.size())
					return Reject(EArchiveFailureCode::InvalidData, "DCOL leaf ordering is incomplete.");
				WireIndices = &OrderedIndices;
				WireOrdinals = &OrderedOrdinals;
			}
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
				Streams[Chunk] = std::make_unique<FCanonicalMemoryWriter>(Buffers[Chunk]);
			TransferStreams();
			Body.reserve(static_cast<size_t>(StoredSize - 64));
			FCanonicalMemoryWriter BodyAr(Body);
			TransferTable(BodyAr);
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				while (BodyAr.Tell() + 64 < Offsets[Chunk]) { uint8 Zero = 0; BodyAr << Zero; }
				BodyAr.WriteBytes(Buffers[Chunk]);
			}
			if (BodyAr.IsError()) return Reject(BodyAr.GetFailure()->Code, BodyAr.GetError());
			Checksum = FXxHash64::HashBuffer(Body).HashValue;
		}

		Ar << Reserved0 << Schema << Builder << Platform << Header << ChunkCount << Alignment << Mode
			<< StoredSize << LogicalBytes << Checksum << Policy << Reserved;
		if (Ar.IsError()) return;
		if (Schema != PhysicsCollisionPayloadSchemaVersion || Builder != PhysicsCookBuilderVersion)
			return Reject(EArchiveFailureCode::UnsupportedVersion, "DCOL schema or builder version is unsupported.");
		if (Platform != static_cast<uint32>(TargetPlatform))
			return Reject(EArchiveFailureCode::UnsupportedTarget, "DCOL target platform does not match.");
		if (Reserved0 != 0 || Header != 64 || ChunkCount != 4
			|| Alignment != PhysicsCollisionPayloadAlignment || Mode != 0 || Policy != 0 || Reserved != 0)
			return Reject(EArchiveFailureCode::InvalidData, "DCOL header layout is invalid.");
		if (StoredSize < 64 + 4 * PhysicsCollisionPayloadChunkEntrySize
			|| StoredSize > MaximumPhysicsCollisionPayloadBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL stored size exceeds its limit.");
		if (Ar.IsSaving())
		{
			Control.Check();
			Ar.WriteBytes(Body);
			return;
		}
		FByteView Region;
		if (!Ar.ReadRegion(StoredSize - 64, Region)) return;
		if (FXxHash64::HashBuffer(Region).HashValue != Checksum)
			return Reject(EArchiveFailureCode::InvalidData, "DCOL checksum does not match.");
		FCanonicalMemoryReader TableAr(Region.first(4 * PhysicsCollisionPayloadChunkEntrySize));
		TransferTable(TableAr);
		if (TableAr.IsError()) return Reject(TableAr.GetFailure()->Code, TableAr.GetError());
		uint64 PreviousEnd = 64 + 4 * PhysicsCollisionPayloadChunkEntrySize, Total = 0;
		for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
		{
			if (Counts[Chunk] > std::numeric_limits<uint64>::max() / ElementSizes[Chunk])
				return Reject(EArchiveFailureCode::Overflow, "DCOL element byte count overflows.");
			if (Offsets[Chunk] % Alignment != 0 || Offsets[Chunk] < PreviousEnd || Offsets[Chunk] > StoredSize
				|| Sizes[Chunk] > StoredSize - Offsets[Chunk]
				|| Counts[Chunk] * ElementSizes[Chunk] != Sizes[Chunk])
				return Reject(EArchiveFailureCode::InvalidData, "DCOL chunk extent or count is invalid.");
			for (uint64 Offset = PreviousEnd; Offset < Offsets[Chunk]; ++Offset)
			{
				Control.Tick();
				if (Region[static_cast<size_t>(Offset - 64)] != std::byte{0})
					return Reject(EArchiveFailureCode::NonZeroPadding, "DCOL padding is nonzero.");
			}
			Streams[Chunk] = std::make_unique<FCanonicalMemoryReader>(Region.subspan(
				static_cast<size_t>(Offsets[Chunk] - 64), static_cast<size_t>(Sizes[Chunk])));
			PreviousEnd = Offsets[Chunk] + Sizes[Chunk];
			Total += Sizes[Chunk];
		}
		if (PreviousEnd != StoredSize) return Reject(EArchiveFailureCode::TrailingData, "DCOL has trailing bytes.");
		if (Counts[0] == 0 || Counts[0] > MaximumPhysicsCollisionVertices || Counts[1] == 0
			|| Counts[1] % 3 != 0 || Counts[2] != Counts[1] / 3 || Counts[2] > 2'000'000 || Total != LogicalBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL logical counts are invalid.");
		const uint64 NativeBytes = Counts[0] * sizeof(FVector3f) + Counts[1] * sizeof(uint32)
			+ Counts[2] * sizeof(uint32) * (Counts[3] ? 2 : 1) + Counts[3] * sizeof(FCollisionGeometryNode);
		if (NativeBytes > MaximumPhysicsCollisionPayloadBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL decoded allocation budget is exceeded.");
		SourceMode = Counts[3] == 0 ? EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			: EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		Control.Check();
		Positions.resize(static_cast<size_t>(Counts[0]));
		Indices.resize(static_cast<size_t>(Counts[1]));
		SourceOrdinals.resize(static_cast<size_t>(Counts[2]));
		Nodes.resize(static_cast<size_t>(Counts[3]));
		LeafTriangles.clear();
		TransferStreams();
		for (auto& Stream : Streams)
			if (!RequireArchiveEnd(*Stream)) return Reject(Stream->GetFailure()->Code, Stream->GetError());
		if (!Nodes.empty()) LeafTriangles = SourceOrdinals;
		FCollisionGeometryRef Validation;
		if (const auto Built = MakePhysicsCollisionGeometry(*this, Validation, ShouldCancel); !Built)
			return Reject(EArchiveFailureCode::InvalidData, FormatPhysicsCollisionPayloadError(Built.error()));
	}
	catch (const FPayloadBuildCancelled&)
	{
		Ar.Fail(EArchiveFailureCode::InvalidData, "Physics payload operation was cancelled.");
	}
}
