#include "PhysicsSharedOutput.h"
#if DURIN_WITH_EDITOR
#include "Physics/PhysicsDerivedData.h"
#include "Asset/CookedAsset.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::PhysicsPrivate
{
	using namespace DerivedData;
	namespace
	{
		constexpr uint64 MaximumMetadataBytes = 24 + 2 * (4 + 48 + 7 * 4);
		struct FArrayDescriptor
		{
			std::string_view Name;
			FSharedByteBuffer FCollisionCookedBlocks::*Member;
			uint64 ElementSize;
		};
		constexpr std::array Arrays{
			FArrayDescriptor{"Vertices", &FCollisionCookedBlocks::Vertices, sizeof(FVector3)},
			FArrayDescriptor{"Triangles", &FCollisionCookedBlocks::Triangles, sizeof(FCollisionGeometryTriangle)},
			FArrayDescriptor{"Nodes", &FCollisionCookedBlocks::Nodes, sizeof(FCollisionGeometryNode)},
			FArrayDescriptor{"LeafTriangles", &FCollisionCookedBlocks::LeafTriangles, sizeof(uint32)},
			FArrayDescriptor{"Planes", &FCollisionCookedBlocks::HullPlanes, sizeof(FCollisionHullPlane)},
			FArrayDescriptor{"HalfEdges", &FCollisionCookedBlocks::HullHalfEdges, sizeof(FCollisionHullHalfEdge)},
			FArrayDescriptor{"Faces", &FCollisionCookedBlocks::HullFaces, sizeof(FCollisionHullFace)}};
		constexpr std::array<std::string_view, 2> Prefixes{"Simple/", "Complex/"};
		auto ValueId(size_t Geometry, size_t Array) -> FValueId
		{ return FValueId::FromName("Durin.Physics.CollisionBlock").MakeIndexed(uint32(Geometry * Arrays.size() + Array)); }
		using FLayout = std::array<std::optional<FCollisionCookedBlocks>, 2>;
		auto ValidSettings(EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy) -> bool
		{
			return (Mode == EBodySetupCollisionSourceMode::None || Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				|| Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
				&& (Policy == EBodySetupCollisionQueryPolicy::SimpleOnly || Policy == EBodySetupCollisionQueryPolicy::ComplexOnly
					|| Policy == EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		}
		auto ActiveArray(size_t Array, size_t Geometry) -> bool
		{
			return Array < 2 || (Geometry == 0 ? Array >= 4 : Array == 2 || Array == 3);
		}
		auto ReadLayout(const FBuildOutput& Output, EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy, const std::function<bool()>& ShouldCancel,
			bool ValidateArrays) -> std::expected<FLayout, std::string>
		{
			if (ShouldCancel && ShouldCancel()) return std::unexpected("Collision output validation was cancelled.");
			const auto Metadata = GetBuildMetadataPayload(Output);
			if (!ValidSettings(Mode, Policy) || Output.GetSchema() != "Physics.CollisionOutput" || Output.GetSchemaVersion() != 2
				|| Metadata.GetSize() > MaximumMetadataBytes
				|| !Output.CheckLimits({.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes}))
				return std::unexpected("Collision output schema, settings or size is invalid.");
			FBinaryReader Reader(Metadata.GetBytes(), {.MaximumTotalBytes = MaximumMetadataBytes});
			uint32 Platform = 0, Profile = 0, SourceMode = 0, QueryPolicy = 0;
			std::array<uint32, 2> Present{};
			if (!Reader.ReadU32(Platform) || !Reader.ReadU32(Profile) || !Reader.ReadU32(SourceMode) || !Reader.ReadU32(QueryPolicy)
				|| !Reader.ReadU32(Present[0]) || !Reader.ReadU32(Present[1])
				|| Platform != uint32(ECookTargetPlatform::Win64) || Profile != uint32(ECookTargetProfile::Game)
				|| SourceMode != uint32(Mode) || QueryPolicy != uint32(Policy)
				|| Present[0] != uint32(Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				|| Present[1] != uint32(Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0))
				return std::unexpected("Collision output header or geometry presence is invalid.");
			FLayout Layout;
			size_t ValueCount = 0;
			for (size_t Geometry = 0; Geometry < 2; ++Geometry)
			{
				if (!Present[Geometry]) continue;
				auto& Blocks = Layout[Geometry].emplace();
				uint32 Kind = 0;
				if (!Reader.ReadU32(Kind) || Kind != uint32(Geometry == 0
					? ECollisionGeometryKind::ConvexHull : ECollisionGeometryKind::TriangleMesh))
					return std::unexpected("Collision output geometry kind is invalid.");
				Blocks.Kind = ECollisionGeometryKind(Kind);
				for (uint32 Axis = 0; Axis < 3; ++Axis)
					if (!Reader.ReadDouble(Blocks.LocalMin[Axis])) return std::unexpected("Collision output bounds are truncated.");
				for (uint32 Axis = 0; Axis < 3; ++Axis)
					if (!Reader.ReadDouble(Blocks.LocalMax[Axis])) return std::unexpected("Collision output bounds are truncated.");
				for (size_t Array = 0; Array < Arrays.size(); ++Array)
				{
					const auto& Descriptor = Arrays[Array];
					uint32 Count = 0;
					if (!Reader.ReadU32(Count)) return std::unexpected("Collision output array count is truncated.");
					const auto* Value = Output.FindValue(ValueId(Geometry, Array));
					if (!ActiveArray(Array, Geometry))
					{
						if (Count || Value) return std::unexpected("Collision output contains an inactive array.");
						continue;
					}
					if (!Count || !Value || Value->GetRawSize() != uint64(Count) * Descriptor.ElementSize)
						return std::unexpected("Collision output array size is invalid.");
					Blocks.*Descriptor.Member = Value->GetData();
					++ValueCount;
				}
				if (ValidateArrays && !FCollisionCookedData::ValidateBlocks(Blocks, ShouldCancel))
					return std::unexpected("Collision output array semantics are invalid or validation was cancelled.");
			}
			if (!Reader.IsAtEnd() || Output.GetValues().size() != ValueCount)
				return std::unexpected("Collision output contains extra metadata or arrays.");
			if (ShouldCancel && ShouldCancel()) return std::unexpected("Collision output validation was cancelled.");
			return Layout;
		}
	}

	auto MakeSharedOutput(FCollisionCookedData Cooked, EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy, const std::function<bool()>& ShouldCancel)
		-> std::expected<FBuildOutput, std::string>
	{
		if (!ValidSettings(Mode, Policy) || bool(Cooked) != (Mode != EBodySetupCollisionSourceMode::None))
			return std::unexpected("Collision recipe presence or settings are invalid.");
		FBuildOutputBuilder Output("Physics.CollisionOutput", 2, {.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes});
		FBinaryWriter Metadata({.MaximumTotalBytes = MaximumMetadataBytes});
		Metadata.WriteU32(uint32(ECookTargetPlatform::Win64)); Metadata.WriteU32(uint32(ECookTargetProfile::Game));
		Metadata.WriteU32(uint32(Mode)); Metadata.WriteU32(uint32(Policy));
		Metadata.WriteU32(Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
		Metadata.WriteU32(Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		if (Cooked)
		{
			const auto Blocks = Cooked.GetBlocks();
			const size_t Geometry = Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? 0 : 1;
			Metadata.WriteU32(uint32(Blocks.Kind));
			for (uint32 Axis = 0; Axis < 3; ++Axis) Metadata.WriteDouble(Blocks.LocalMin[Axis]);
			for (uint32 Axis = 0; Axis < 3; ++Axis) Metadata.WriteDouble(Blocks.LocalMax[Axis]);
			for (size_t Array = 0; Array < Arrays.size(); ++Array)
			{
				const auto& Descriptor = Arrays[Array];
				const auto& Block = Blocks.*Descriptor.Member;
				if (Block.GetSize() % Descriptor.ElementSize || Block.GetSize() / Descriptor.ElementSize > std::numeric_limits<uint32>::max())
					return std::unexpected("Collision recipe array size exceeds its bound.");
				Metadata.WriteU32(uint32(Block.GetSize() / Descriptor.ElementSize));
				if (ActiveArray(Array, Geometry)) Output.AddValue(ValueId(Geometry, Array), Block);
			}
		}
		if (Metadata.HasError()) return std::unexpected("Collision metadata exceeds its bound.");
		auto Meta = MakeBuildMetadata(FSharedByteBuffer::Take(Metadata.TakeBytes()));
		if (!Meta || !Output.AddMeta(FValueId::FromName("Metadata"), std::move(*Meta))) return std::unexpected("Collision metadata is invalid.");
		auto Built = std::move(Output).Build(); if (!Built) return Built;
		if (auto Valid = ValidateSharedOutput(*Built, Mode, Policy, ShouldCancel); !Valid) return std::unexpected(std::move(Valid.error()));
		return Built;
	}

	auto ValidateSharedOutput(const FBuildOutput& Output, EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy, const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
	{
		auto Layout = ReadLayout(Output, Mode, Policy, ShouldCancel, true);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		return {};
	}

	auto AssembleSharedOutput(const FBuildOutput& Output, EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy, const std::function<bool()>& ShouldCancel) -> std::expected<FPhysicsCookResult, std::string>
	{
		bool bCancelled = false;
		const auto Cancel = [&] { bCancelled = bCancelled || (ShouldCancel && ShouldCancel()); return bCancelled; };
		// FromBlocks validates arrays immediately before conversion; do not repeat it here.
		auto Layout = ReadLayout(Output, Mode, Policy, Cancel, false);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		FPhysicsCookResult Result;
		for (size_t Geometry = 0; Geometry < 2; ++Geometry)
		{
			if (!(*Layout)[Geometry]) continue;
			auto Cooked = FCollisionCookedData::FromBlocks(std::move(*(*Layout)[Geometry]), Cancel);
			if (!Cooked) return std::unexpected("Collision output assembly failed validation or was cancelled.");
			auto Published = FCollisionGeometryRef::MakeCooked(std::move(Cooked), Cancel);
			if (!Published) return std::unexpected("Collision output publication was cancelled.");
			(Geometry == 0 ? Result.Simple : Result.Complex) = std::move(Published);
		}
		if (Cancel()) return std::unexpected("Collision output assembly was cancelled.");
		return Result;
	}
}
#endif
