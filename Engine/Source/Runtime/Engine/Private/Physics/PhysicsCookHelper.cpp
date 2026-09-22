#include "Physics/PhysicsCookDerivedDataKey.h"
#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsCookHelper.h"
#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		auto BuildCollisionGeometryHash(std::span<const FVector3f> Positions,
			std::span<const uint32> Indices, const std::function<bool()>& ShouldCancel) -> std::optional<FXxHash128>
		{
			FXxHash128Builder Hash;
			FBinaryWriter PositionHeader;
			PositionHeader.WriteU64(Positions.size());
			Hash.Update(PositionHeader.GetBytes());
			for (size_t Begin = 0; Begin < Positions.size(); Begin += 256)
			{
				if (ShouldCancel()) return {};
				FBinaryWriter Block;
				for (size_t Index = Begin; Index < std::min(Begin + 256, Positions.size()); ++Index)
					for (uint32 Axis = 0; Axis < 3; ++Axis)
						Block.WriteU32(std::bit_cast<uint32>(Positions[Index][Axis]));
				Hash.Update(Block.GetBytes());
			}
			FBinaryWriter IndexHeader;
			IndexHeader.WriteU64(Indices.size());
			Hash.Update(IndexHeader.GetBytes());
			for (size_t Begin = 0; Begin < Indices.size(); Begin += 256)
			{
				if (ShouldCancel()) return {};
				FBinaryWriter Block;
				for (size_t Index = Begin; Index < std::min(Begin + 256, Indices.size()); ++Index)
					Block.WriteU32(Indices[Index]);
				Hash.Update(Block.GetBytes());
			}
			return Hash.Finalize();
		}

		auto ArchiveCodecFailure(const FArchive& Ar) -> std::expected<void, std::string>
		{
			const auto* Failure = Ar.GetFailure();
			return std::unexpected(std::format("Payload archive failed at byte {} (Archive code {}, path '{}'): {}",
				Ar.Tell(), Failure ? static_cast<int>(Failure->Code) : -1,
				Failure ? Failure->Path : std::string{}, Ar.GetError()));
		}

		auto EncodeCollision(const FCollisionGeometryRef& Geometry, EBodySetupCollisionQueryPolicy Policy,
			FByteBuffer& OutBytes, const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FPhysicsCollisionPayloadData Payload;
			if (const auto Built = MakePhysicsCollisionPayloadData(Geometry, Policy, Payload, ShouldCancel); !Built)
				return std::unexpected(FormatPhysicsCollisionPayloadError(Built.error()));
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (!Ar.IsError()) return {};
			const auto Failure = ArchiveCodecFailure(Ar);
			OutBytes.clear();
			return Failure;
		}

		auto DecodeCollision(FByteView Bytes, EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy, FCollisionGeometryRef& OutGeometry,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FPhysicsCollisionPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return ArchiveCodecFailure(Ar);
			if (Payload.SourceMode != Mode || Payload.QueryPolicy != Policy)
				return std::unexpected(std::format("Cached physics mode/policy ({}/{}) does not match ({}/{}).", static_cast<int>(Payload.SourceMode), static_cast<int>(Payload.QueryPolicy), static_cast<int>(Mode), static_cast<int>(Policy)));
			if (const auto Built = MakePhysicsCollisionGeometry(Payload, OutGeometry, ShouldCancel); !Built)
				return std::unexpected(FormatPhysicsCollisionPayloadError(Built.error()));
			return {};
		}


	}
#endif
	auto FPhysicsCookHelper::Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>
	{
		const auto Mode = Info.Mode;
		const auto Policy = Info.Policy;
		bool bCancelled = false;
		const auto IsCancelled = [&] { bCancelled = bCancelled || Control.IsCancelled(); return bCancelled; };
		const auto Fail = [](std::string Message) -> std::expected<FPhysicsCookResult, FPhysicsCookFailure> {
			return std::unexpected(FPhysicsCookFailure{std::move(Message), EPhysicsCookStage::Cook});
		};
		if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
			return FPhysicsCookResult{};
		}
		if (Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0 && Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			return Fail("Physics cook source mode is invalid.");
		if (Policy != EBodySetupCollisionQueryPolicy::SimpleOnly && Policy != EBodySetupCollisionQueryPolicy::ComplexOnly
			&& Policy != EBodySetupCollisionQueryPolicy::SimpleAndComplex) return Fail("Physics cook query policy is invalid.");
		const auto& Positions = Info.TriangleMeshDesc.Positions;
		const auto& Indices = Info.TriangleMeshDesc.Indices;
		FAssetBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Positions.capacity(), 512) || !Memory.Add(Indices.capacity(), 192))
			return Fail("Physics cook input and working set exceed the reservation.");
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0)
			return Fail(std::format("Physics cook input is malformed ({} vertices, {} indices).", Positions.size(), Indices.size()));
		FCollisionGeometryRef Geometry;
#if DURIN_WITH_EDITOR
		const auto GeometryHash = BuildCollisionGeometryHash(Positions, Indices, IsCancelled);
		if (!GeometryHash) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
		const auto KeyResult = BuildPhysicsCookDerivedDataKey({.GeometryHash = *GeometryHash,
			.SourceMode = Mode, .QueryPolicy = Policy, .BuilderVersion = PhysicsCookBuilderVersion,
			.TargetPlatform = EAssetPayloadTargetPlatform::Win64});
		if (!KeyResult) return Fail(FormatPhysicsCookKeyError(KeyResult.error()));
		const auto Key = *KeyResult;
		FByteBuffer Bytes;
		AssetDerivedDataCache::FOperationDiagnostic LoadDiagnostic, StoreDiagnostic;
		std::optional<std::string> DecodeCause;
		if (AssetDerivedDataCache::Load(Key, std::min(MaximumPhysicsCollisionPayloadBytes,
			Control.MaximumWorkingSetBytes / 16), Bytes, LoadDiagnostic) == AssetDerivedDataCache::ELoadResult::Hit)
		{
			const auto Decoded = DecodeCollision(Bytes, Mode, Policy, Geometry, IsCancelled);
			if (!Decoded) { DecodeCause = Decoded.error(); Geometry = {}; }
		}
#endif
		if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
		if (!Geometry)
		{
			std::vector<FVector3> CollisionPositions;
			CollisionPositions.reserve(Positions.size());
			for (size_t Index = 0; Index < Positions.size(); ++Index)
			{
				if (Index % 256 == 0 && IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
				CollisionPositions.emplace_back(Positions[Index]);
			}
			FCollisionGeometryBuildDiagnostics Diagnostics;
			Geometry = Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				? FCollisionGeometryRef::BuildConvexHull(CollisionPositions, &Diagnostics, IsCancelled)
				: FCollisionGeometryRef::BuildTriangleMesh(CollisionPositions, Indices, &Diagnostics, IsCancelled);
			if (Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled || IsCancelled())
				return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
			if (!Geometry) return Fail(std::format("Physics geometry construction failed (status {}).", static_cast<int>(Diagnostics.Status)));
#if DURIN_WITH_EDITOR
			if (const auto Encoded = EncodeCollision(Geometry, Policy, Bytes, IsCancelled); !Encoded)
			{
				if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
				return Fail(Encoded.error());
			}
			if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
			if (Info.bPersistDerivedData) AssetDerivedDataCache::Store(Key, Bytes, MaximumPhysicsCollisionPayloadBytes, StoreDiagnostic);
#endif
		}
		if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Cook));
		FPhysicsCookResult Result;
		if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0) Result.Simple = std::move(Geometry);
		else Result.Complex = std::move(Geometry);
#if DURIN_WITH_EDITOR
		Result.CacheWarnings = AssetDerivedDataCache::CollectBuildWarnings(LoadDiagnostic, StoreDiagnostic, DecodeCause);
#endif
		return Result;
	}
}
