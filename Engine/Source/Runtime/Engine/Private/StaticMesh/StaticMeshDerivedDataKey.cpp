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
	auto MakeStaticMeshSessionDefinition(uint32 MaterialSlotCount, uint64 BuilderVersion)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>
	{
		if (!BuilderVersion) return std::unexpected(DerivedData::FBuildDefinitionError{DerivedData::EBuildDefinitionError::InvalidConstant, "BuilderVersion"});
		DerivedData::FBuildDefinitionBuilder Builder("Durin.StaticMesh.Render");
		Builder.AddConstant("BuilderVersion", BuilderVersion);
		Builder.AddConstant("TargetPlatform", uint64(EAssetPayloadTargetPlatform::Win64))
			.AddConstant("MaterialSlotCount", uint64(MaterialSlotCount)).AddInput("Source", "CapturedSource")
			.AddInput("Reconciliation", "CapturedReconciliation");
		return std::move(Builder).Build();
	}
	auto GetStaticMeshBuildDescriptor(uint32 FunctionVersion, uint32 OutputVersion) -> DerivedData::FBuildFunctionDescriptor
	{
		return {"Durin.StaticMesh.Render", FunctionVersion, 2, "StaticMesh.RenderOutput", OutputVersion,
			DerivedData::FCacheBucket::FromString("StaticMesh")};
	}

	auto MakeStaticMeshBuildAction(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FStaticMeshBuildKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Request = MakeStaticMeshSessionDefinition(Input.MaterialSlotCount, Input.BuilderVersion);
		if (!Request) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		FBuildActionBuilder Builder(*Request, GetStaticMeshBuildDescriptor(Input.FunctionVersion, Input.OutputSchemaVersion));
		Builder.AddInput({"Source", Input.SourceHash, "StaticMeshSource", StaticMeshSourceGeometryIdentityVersion, "StaticMesh.AuthoredGeometry", 1})
			.AddInput({"Reconciliation", Input.ReconciliationHash, "StaticMeshReconciliation", 1, "StaticMesh.MaterialSlots", 1});
		auto Definition = std::move(Builder).Build();
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
