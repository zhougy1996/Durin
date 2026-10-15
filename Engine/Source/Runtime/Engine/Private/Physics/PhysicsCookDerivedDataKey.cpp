#include "Physics/PhysicsCookDerivedDataKey.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildDefinition.h"
#include "StaticMesh/StaticMeshSource.h"
#include "Serialization/Archive.h"
#include "TargetPlatform.h"

namespace Durin
{
	auto MakePhysicsCookSessionDefinition(EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		uint64 BuilderVersion, uint32 WeldToleranceBits, EAssetPayloadTargetPlatform TargetPlatform) -> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>
	{
		const auto Target = ResolveTargetPlatform(static_cast<ECookTargetPlatform>(TargetPlatform));
		if (!Target || !Target->SupportsCollisionPayload())
			return std::unexpected(DerivedData::FBuildDefinitionError{DerivedData::EBuildDefinitionError::InvalidConstant, "TargetPlatform"});
		if (!BuilderVersion) return std::unexpected(DerivedData::FBuildDefinitionError{DerivedData::EBuildDefinitionError::InvalidConstant, "BuilderVersion"});
		DerivedData::FBuildDefinitionBuilder Builder("Durin.Physics.Collision");
		Builder.AddConstant("BuilderVersion", BuilderVersion);
		Builder.AddConstant("TargetPlatform", uint64(TargetPlatform))
			.AddConstant("SourceMode", uint64(Mode)).AddConstant("QueryPolicy", uint64(Policy))
			.AddConstant("WeldToleranceBits", uint64(WeldToleranceBits)).AddInput("Geometry", "CapturedGeometry");
		return std::move(Builder).Build();
	}
	auto GetPhysicsCookBuildDescriptor(uint32 FunctionVersion, uint32 OutputVersion) -> DerivedData::FBuildFunctionDescriptor
	{
		return {"Durin.Physics.Collision", FunctionVersion, PhysicsCollisionConstantsSchemaVersion, "Physics.CollisionOutput", OutputVersion,
			DerivedData::FCacheBucket::FromString(PhysicsCollisionCacheBucket)};
	}

	auto MakePhysicsCookBuildAction(const FPhysicsCookKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FPhysicsCookKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Request = MakePhysicsCookSessionDefinition(Input.SourceMode, Input.QueryPolicy, Input.BuilderVersion, Input.WeldToleranceBits, Input.TargetPlatform);
		if (!Request) return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		FBuildActionBuilder Builder(*Request, GetPhysicsCookBuildDescriptor(Input.FunctionVersion, Input.OutputSchemaVersion));
		Builder.AddInput({"Geometry", Input.GeometryHash, "CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1});
		auto Definition = std::move(Builder).Build();
		if (!Definition) return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

}
#endif
