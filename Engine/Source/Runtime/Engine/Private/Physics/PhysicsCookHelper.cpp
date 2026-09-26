#include "Physics/PhysicsCookDerivedDataKey.h"
#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsCookHelper.h"
#include "Asset/AssetDerivedDataBuild.h"
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

		template<typename TBuild>
		struct TPhysicsBuildAdapter
		{
			using FProduct = FCollisionGeometryRef;
			using FError = FPhysicsCookFailure;
			const FCookBodySetupInfo& Input;
			const DerivedData::FBuildDefinition& Definition;
			std::array<DerivedData::FBuildInputReference, 1> Inputs;
			std::function<bool()> ShouldCancel;
			TBuild BuildGeometry;
			auto GetFunction() const -> DerivedData::FBuildFunctionDescriptor
			{
				auto Function = Definition.GetFunction();
				Function.Version = PhysicsCookBuilderVersion;
				return Function;
			}
			auto GetInputs() const -> std::span<const DerivedData::FBuildInputReference> { return Inputs; }
			auto MakeError(DerivedData::EBuildFailure Code) const -> FError
			{
				return Code == DerivedData::EBuildFailure::Cancelled ? FError::Cancelled()
					: FError{"Physics build definition does not match its input or producer."};
			}
			auto IsCancelled(const FError& Error) const -> bool { return Error.IsCancelled(); }
			auto ValidateBindings(const DerivedData::FBuildDefinition&) const -> std::expected<void, FError> { return {}; }
			auto Resolve() const -> std::expected<std::reference_wrapper<const FCookBodySetupInfo>, FError> { return std::cref(Input); }
			auto Build(std::reference_wrapper<const FCookBodySetupInfo>&) const -> std::expected<FProduct, FError> { return BuildGeometry(); }
			auto Validate(const FProduct& Product) const -> std::expected<void, FError>
			{
				if (!Product.IsValid()) return std::unexpected(FError{"Physics builder returned invalid collision geometry."});
				return {};
			}
			auto Decode(const FSharedByteBuffer& Bytes) const -> std::expected<FProduct, FError>
			{
				FProduct Product;
				auto Decoded = DecodeCollision(Bytes.GetBytes(), Input.Mode, Input.Policy, Product, ShouldCancel);
				if (!Decoded) return std::unexpected(ShouldCancel() ? FError::Cancelled() : FError{Decoded.error()});
				return Product;
			}
			auto Encode(FProduct& Product) const -> std::expected<FByteBuffer, FError>
			{
				FByteBuffer Bytes;
				auto Encoded = EncodeCollision(Product, Input.Policy, Bytes, ShouldCancel);
				if (!Encoded) return std::unexpected(ShouldCancel() ? FError::Cancelled() : FError{Encoded.error()});
				return Bytes;
			}
		};

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
		auto BuildGeometry = [&]() -> std::expected<FCollisionGeometryRef, FPhysicsCookFailure> {
			std::vector<FVector3> CollisionPositions;
			CollisionPositions.reserve(Positions.size());
			for (size_t Index = 0; Index < Positions.size(); ++Index)
			{
				if (Index % 256 == 0 && IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled());
				CollisionPositions.emplace_back(Positions[Index]);
			}
			FCollisionGeometryBuildDiagnostics Diagnostics;
			auto Geometry = Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				? FCollisionGeometryRef::BuildConvexHull(CollisionPositions, &Diagnostics, IsCancelled)
				: FCollisionGeometryRef::BuildTriangleMesh(CollisionPositions, Indices, &Diagnostics, IsCancelled);
			if (Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled || IsCancelled())
				return std::unexpected(FPhysicsCookFailure::Cancelled());
			if (!Geometry) return std::unexpected(FPhysicsCookFailure{
				std::format("Physics geometry construction failed (status {}).", static_cast<int>(Diagnostics.Status))});
			return Geometry;
		};
#if DURIN_WITH_EDITOR
		const auto GeometryHash = BuildCollisionGeometryHash(Positions, Indices, IsCancelled);
		if (!GeometryHash) return std::unexpected(FPhysicsCookFailure::Cancelled());
		auto Definition = MakePhysicsCookBuildDefinition({.GeometryHash = *GeometryHash,
			.SourceMode = Mode, .QueryPolicy = Policy, .BuilderVersion = PhysicsCookBuilderVersion,
			.TargetPlatform = EAssetPayloadTargetPlatform::Win64});
		if (!Definition) return Fail(FormatPhysicsCookKeyError(Definition.error()));
		TPhysicsBuildAdapter<decltype(BuildGeometry)> Adapter{.Input = Info, .Definition = *Definition,
			.Inputs = {DerivedData::FBuildInputReference{"Geometry", *GeometryHash,
				"CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1}},
			.ShouldCancel = IsCancelled, .BuildGeometry = BuildGeometry};
		DerivedData::TBuildObservations<FPhysicsCookFailure> Observations;
		const uint64 MaximumBytes = std::min(MaximumPhysicsCollisionPayloadBytes, Control.MaximumWorkingSetBytes / 16);
		auto Built = DerivedData::ExecuteBuild(*Definition, Adapter,
			{.bWriteCache = Info.bPersistDerivedData, .MaximumValueBytes = MaximumBytes},
			{.ShouldCancel = IsCancelled}, Observations);
		AssetDerivedDataBuild::ReportCacheIssues(*Definition, Observations,
			[](const FPhysicsCookFailure& Error) { return Error.ToString(); });
#else
		auto Built = BuildGeometry();
#endif
		if (!Built) return std::unexpected(std::move(Built.error()));
		if (IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled());
		auto Geometry = std::move(*Built);
		FPhysicsCookResult Result;
		if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0) Result.Simple = std::move(Geometry);
		else Result.Complex = std::move(Geometry);
		return Result;
	}
}
