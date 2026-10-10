#include "StaticMesh/StaticMeshDerivedDataKey.h"

#if DURIN_WITH_EDITOR

#include "DerivedDataBuildDefinition.h"

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
	auto GetStaticMeshBuildDescriptor() -> DerivedData::FBuildFunctionDescriptor
	{
		return {"Durin.StaticMesh.Render", StaticMeshRenderBuildFunctionVersion, StaticMeshRenderConstantsSchemaVersion,
			std::string(StaticMeshRenderOutputType), StaticMeshRenderOutputSchemaVersion,
			DerivedData::FCacheBucket::FromString("StaticMesh")};
	}


}
#endif
