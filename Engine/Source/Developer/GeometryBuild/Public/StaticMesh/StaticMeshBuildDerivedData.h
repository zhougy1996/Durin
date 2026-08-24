#pragma once

#include "GeometryBuildAPI.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin::Asset::Build
{
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		FXxHash128 ReconciliationHash;
		std::string ImporterId;
		uint32 ImporterVersion = 0;
		FStaticMeshImportSettings ImportSettings;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		GEOMETRYBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	struct FStaticMeshCollisionBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		FXxHash128 GeometryHash;
		std::string ImporterId;
		uint32 ImporterVersion = 0;
		FStaticMeshImportSettings ImportSettings;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint32 WeldToleranceBits = 0;
		uint32 BuilderVersion = StaticMeshCollisionBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		GEOMETRYBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	GEOMETRYBUILD_API auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>;
	GEOMETRYBUILD_API auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string;
	GEOMETRYBUILD_API auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>;
	GEOMETRYBUILD_API auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
