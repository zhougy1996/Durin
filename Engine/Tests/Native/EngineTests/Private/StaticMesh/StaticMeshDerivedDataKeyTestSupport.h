#pragma once

#include "CoreMinimal.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshSource.h"
#include "Serialization/Archive.h"

namespace Durin::Testing
{
	enum class EStaticMeshBuildKeyError : uint8 { None, UnsupportedTarget, Archive };
	struct FStaticMeshBuildKeyError
	{
		EStaticMeshBuildKeyError Code = EStaticMeshBuildKeyError::None;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;
		std::optional<EArchiveFailureCode> ArchiveCode;
	};

	// Explicit action fixture for key contracts and cache corruption tests.
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 SourceHash;
		FXxHash128 ReconciliationHash;
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = StaticMeshRenderBuildFunctionVersion;
		uint32 OutputSchemaVersion = StaticMeshRenderOutputSchemaVersion;
		uint32 MaterialSlotCount = 1;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

	};

	inline auto MakeStaticMeshBuildAction(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FStaticMeshBuildKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Request = MakeStaticMeshSessionDefinition(Input.MaterialSlotCount, Input.BuilderVersion);
		if (!Request) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		auto Descriptor = GetStaticMeshBuildDescriptor();
		Descriptor.Version = Input.FunctionVersion;
		Descriptor.OutputSchema = Input.OutputSchemaVersion;
		FBuildActionBuilder Builder(*Request, std::move(Descriptor));
		Builder.AddInput({"Source", Input.SourceHash, "StaticMeshSource", StaticMeshSourceGeometryIdentityVersion, "StaticMesh.AuthoredGeometry", 1})
			.AddInput({"Reconciliation", Input.ReconciliationHash, "StaticMeshReconciliation", 1, "StaticMesh.MaterialSlots", 1});
		auto Definition = std::move(Builder).Build();
		if (!Definition) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

	inline auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
	}
	inline auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FCacheKeyProxy(Definition->GetKey());
	}
}
