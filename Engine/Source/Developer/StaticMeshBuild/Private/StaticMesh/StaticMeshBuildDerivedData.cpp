#include "StaticMesh/StaticMeshBuildDerivedData.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
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
			std::string& OutError) -> FByteArray
		{
			FByteArray Bytes;
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
		std::string& OutError) -> FByteArray
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const FByteArray Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FByteArray
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const FByteArray Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
