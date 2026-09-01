#pragma once

#include "StaticMeshBuildAPI.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 ImportedDataHash;
		FXxHash128 ReconciliationHash;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		STATICMESHBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	struct FStaticMeshCollisionBuildKeyInput
	{
		FXxHash128 GeometryHash;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint32 WeldToleranceBits = 0;
		uint32 BuilderVersion = StaticMeshCollisionBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		STATICMESHBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	STATICMESHBUILD_API auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	STATICMESHBUILD_API auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string;
	STATICMESHBUILD_API auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	STATICMESHBUILD_API auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
