#include "StaticMesh/StaticMeshDerivedDataKey.h"

#if DURIN_WITH_EDITOR

#include "DerivedDataBuildDefinition.h"
#include "StaticMesh/StaticMeshSource.h"

#include "Serialization/Archive.h"
#include "Hash/CanonicalHash.h"

namespace Durin
{
	template<typename TSlot>
	auto HashReconciliation(
		std::span<const TSlot> MaterialSlots,
		float NormalizedSize) -> FXxHash128
	{
		FXxHash128Builder Builder;
		UpdateCanonicalHash(Builder, std::bit_cast<uint32>(NormalizedSize));
		UpdateCanonicalHash(Builder, static_cast<uint64>(MaterialSlots.size()));
		for (const auto& Slot : MaterialSlots)
		{
			UpdateCanonicalHashString(Builder, Slot.Name.ToString());
			UpdateCanonicalHashString(Builder, Slot.SourceName);
			UpdateCanonicalHash(Builder, Slot.SourceMaterialIndex);
		}
		return Builder.Finalize();
	}
	auto BuildStaticMeshReconciliationHash(std::span<const FMeshMaterialSlotDefinition> Slots, float Size) -> FXxHash128
	{ return HashReconciliation(Slots, Size); }
	auto BuildStaticMeshReconciliationHash(std::span<const FStaticMeshBuildMaterialSlot> Slots, float Size) -> FXxHash128
	{ return HashReconciliation(Slots, Size); }
	auto MakeStaticMeshSessionDefinition(uint32 MaterialSlotCount)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>
	{
		return DerivedData::FBuildDefinition::TryCreate("Durin.StaticMesh.Render",
			{{"TargetPlatform", uint64(EAssetPayloadTargetPlatform::Win64)}, {"MaterialSlotCount", uint64(MaterialSlotCount)}},
			{{"Source", "CapturedSource"}, {"Reconciliation", "CapturedReconciliation"}});
	}
	auto GetStaticMeshBuildDescriptor(uint32 BuilderVersion, uint32 OutputVersion) -> DerivedData::FBuildFunctionDescriptor
	{
		return {"Durin.StaticMesh.Render", BuilderVersion, 1, "StaticMesh.RenderOutput", OutputVersion,
			DerivedData::FCacheBucket::FromString(StaticMeshCacheBucket)};
	}

	auto FormatStaticMeshBuildKeyError(const FStaticMeshBuildKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshBuildKeyError::None: return {};
		case EStaticMeshBuildKeyError::UnsupportedTarget:
			return std::format("StaticMesh derived-data target {} is unsupported.", static_cast<uint32>(Error.TargetPlatform));
		case EStaticMeshBuildKeyError::Archive:
			return std::format("StaticMesh derived-data key encoding failed (Archive code {}, path '{}').",
				Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		}
		return {};
	}

	auto MakeStaticMeshBuildAction(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FStaticMeshBuildKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Request = MakeStaticMeshSessionDefinition(Input.MaterialSlotCount);
		if (!Request) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		auto Definition = FBuildAction::TryCreate(*Request, GetStaticMeshBuildDescriptor(Input.BuilderVersion, Input.OutputSchemaVersion),
			{{"Source", Input.SourceHash, "StaticMeshSource", StaticMeshSourceGeometryPayloadVersion, "StaticMesh.AuthoredGeometry", 1},
			 {"Reconciliation", Input.ReconciliationHash, "StaticMeshReconciliation", 1, "StaticMesh.MaterialSlots", 1}});
		if (!Definition) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

	auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
	}
	auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildAction(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FCacheKeyProxy(Definition->GetKey());
	}
}
#endif
