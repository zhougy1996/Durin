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

	auto MakePhysicsCookSessionDefinition(EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		uint32 WeldToleranceBits) -> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>
	{
		return DerivedData::FBuildDefinition::TryCreate("Durin.Physics.Collision",
			{{"TargetPlatform", uint64(EAssetPayloadTargetPlatform::Win64)}, {"SourceMode", uint64(Mode)},
			 {"QueryPolicy", uint64(Policy)}, {"WeldToleranceBits", uint64(WeldToleranceBits)}}, {{"Geometry", "CapturedGeometry"}});
	}
	auto GetPhysicsCookBuildDescriptor(uint32 BuilderVersion, uint32 OutputVersion) -> DerivedData::FBuildFunctionDescriptor
	{
		return {"Durin.Physics.Collision", BuilderVersion, 1, "Physics.CollisionOutput", OutputVersion,
			DerivedData::FCacheBucket::FromString(PhysicsCollisionCacheBucket)};
	}

	auto MakePhysicsCookBuildAction(const FPhysicsCookKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FPhysicsCookKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Request = MakePhysicsCookSessionDefinition(Input.SourceMode, Input.QueryPolicy, Input.WeldToleranceBits);
		if (!Request) return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		auto Definition = FBuildAction::TryCreate(*Request, GetPhysicsCookBuildDescriptor(Input.BuilderVersion, Input.OutputSchemaVersion),
			{{"Geometry", Input.GeometryHash, "CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1}});
		if (!Definition) return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

	auto BuildPhysicsCookDerivedDataKeyBytes(const FPhysicsCookKeyInput& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>
	{
		auto Definition = MakePhysicsCookBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
	}
	auto BuildPhysicsCookDerivedDataKey(const FPhysicsCookKeyInput& Input) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>
	{
		auto Definition = MakePhysicsCookBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FCacheKeyProxy(Definition->GetKey());
	}
}
#endif
