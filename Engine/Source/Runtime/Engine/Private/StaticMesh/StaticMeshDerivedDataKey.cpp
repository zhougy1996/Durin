#include "StaticMesh/StaticMeshDerivedDataKey.h"

#if DURIN_WITH_EDITOR

#include "DerivedDataCache/DerivedDataCache.h"

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
		auto BuildKeyBytes(const T& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
		{
			if (Input.TargetPlatform != EStaticMeshTargetPlatform::Win64)
				return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			if (Ar.IsError())
				return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive, .TargetPlatform = Input.TargetPlatform,
					.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path});
			return Bytes;
		}

		template<typename T>
		auto BuildKey(const T& Input, std::string_view Bucket) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
		{
			auto Encoded = BuildKeyBytes(Input);
			if (!Encoded) return std::unexpected(std::move(Encoded.error()));
			return FCacheKeyProxy(DerivedData::FCacheKey::FromHash(
				DerivedData::FCacheBucket::FromString(Bucket), FXxHash128::HashBuffer(*Encoded)));
		}

	}

	auto FStaticMeshBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateTargetPlatform(Ar, TargetPlatform)) return;
		uint32 KeySchemaVersion = StaticMeshDerivedDataKeySchemaVersion;
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << SourceHash.HashLow << SourceHash.HashHigh
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

	auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
	{
		return BuildKeyBytes(Input);
	}
	auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
	{
		return BuildKey(Input, StaticMeshCacheBucket);
	}
	auto BuildStaticMeshCollisionDerivedDataKeyBytes(const FStaticMeshCollisionBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
	{
		return BuildKeyBytes(Input);
	}
	auto BuildStaticMeshCollisionDerivedDataKey(const FStaticMeshCollisionBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
	{
		return BuildKey(Input, StaticMeshCollisionCacheBucket);
	}
}

#endif
