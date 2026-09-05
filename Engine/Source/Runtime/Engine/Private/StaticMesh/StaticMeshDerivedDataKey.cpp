#include "StaticMesh/StaticMeshDerivedDataKey.h"

#include "DerivedDataCache/DerivedDataCache.h"

#if DURIN_WITH_EDITOR

#include "Serialization/Archive.h"

namespace Durin
{
	auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(NormalizedSize);
		const uint64 SlotCount = MaterialSlots.size();
		Builder.UpdateValue(SlotCount);
		for (const FMeshMaterialSlotDefinition& Slot : MaterialSlots)
		{
			const std::string Name = Slot.Name.ToString();
			const uint64 NameSize = Name.size();
			Builder.UpdateValue(NameSize);
			Builder.Update(Name);
			const uint64 SourceNameSize = Slot.SourceName.size();
			Builder.UpdateValue(SourceNameSize);
			Builder.Update(Slot.SourceName);
			Builder.UpdateValue(Slot.SourceMaterialIndex);
		}
		return Builder.Finalize();
	}
	namespace
	{
		auto ValidateTargetPlatform(
			FArchive& Ar,
			EStaticMeshTargetPlatform TargetPlatform) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"StaticMesh build-key input is save-only.");
				return false;
			}
			if (TargetPlatform != EStaticMeshTargetPlatform::Win64)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh derived-data target is unsupported.");
				return false;
			}
			return true;
		}

		template<typename T>
		auto BuildKeyBytes(
			const T& Input,
			std::string& OutError) -> FByteBuffer
		{
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
			if (Ar.HasError()) Bytes.clear();
			return Bytes;
		}
	}

	auto FStaticMeshBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateTargetPlatform(Ar, TargetPlatform)) return;
		uint32 KeySchemaVersion = StaticMeshDerivedDataKeySchemaVersion;
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << ImportedDataHash.HashLow << ImportedDataHash.HashHigh
			<< ReconciliationHash.HashLow << ReconciliationHash.HashHigh
			<< BuilderVersion << PayloadSchemaVersion << Platform;
	}

	auto FStaticMeshCollisionBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateTargetPlatform(Ar, TargetPlatform)) return;
		uint32 KeySchemaVersion = StaticMeshCollisionKeySchemaVersion;
		uint8 Mode = static_cast<uint8>(SourceMode);
		uint8 Policy = static_cast<uint8>(QueryPolicy);
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << GeometryHash.HashLow << GeometryHash.HashHigh
			<< Mode << Policy
			<< WeldToleranceBits << BuilderVersion << PayloadSchemaVersion << Platform;
	}

	auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> FByteBuffer
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> FCacheKeyProxy
	{
		const FByteBuffer Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? FCacheKeyProxy{}
			: FCacheKeyProxy(DerivedData::FCacheKey::FromHash(
				DerivedData::FCacheBucket::FromString(StaticMeshCacheBucket),
				FXxHash128::HashBuffer(Bytes)));
	}

	auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FByteBuffer
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FCacheKeyProxy
	{
		const FByteBuffer Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? FCacheKeyProxy{}
			: FCacheKeyProxy(DerivedData::FCacheKey::FromHash(
				DerivedData::FCacheBucket::FromString(
					StaticMeshCollisionCacheBucket),
				FXxHash128::HashBuffer(Bytes)));
	}
}

#endif
