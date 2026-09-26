#include "Physics/PhysicsCookDerivedDataKey.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildDefinition.h"
#include "StaticMesh/StaticMeshSource.h"
#include "Serialization/Archive.h"

namespace Durin
{
	auto FormatPhysicsCookKeyError(const FPhysicsCookKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPhysicsCookKeyError::None: return {};
		case EPhysicsCookKeyError::UnsupportedTarget:
			return std::format("Physics derived-data target {} is unsupported.", static_cast<uint32>(Error.TargetPlatform));
		case EPhysicsCookKeyError::Archive:
			return std::format("Physics derived-data key encoding failed (Archive code {}, path '{}').",
				Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		}
		return {};
	}

	auto MakePhysicsCookBuildDefinition(const FPhysicsCookKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, FPhysicsCookKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Definition = FBuildDefinition::TryCreate({"Durin.Physics.Collision", Input.BuilderVersion, 1,
			"Physics.Collision", Input.PayloadSchemaVersion, FCacheBucket::FromString(PhysicsCollisionCacheBucket)},
			{{"TargetPlatform", uint64(Input.TargetPlatform)}, {"SourceMode", uint64(Input.SourceMode)},
			 {"QueryPolicy", uint64(Input.QueryPolicy)}, {"WeldToleranceBits", uint64(Input.WeldToleranceBits)}},
			{{"Geometry", Input.GeometryHash, "CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1}});
		if (!Definition) return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

	auto BuildPhysicsCookDerivedDataKeyBytes(const FPhysicsCookKeyInput& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>
	{
		auto Definition = MakePhysicsCookBuildDefinition(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
	}
	auto BuildPhysicsCookDerivedDataKey(const FPhysicsCookKeyInput& Input) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>
	{
		auto Definition = MakePhysicsCookBuildDefinition(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FCacheKeyProxy(Definition->GetKey());
	}
}
#endif
