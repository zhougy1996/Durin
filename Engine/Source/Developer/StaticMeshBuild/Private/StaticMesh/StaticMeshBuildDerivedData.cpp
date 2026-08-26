#include "StaticMesh/StaticMeshBuildDerivedData.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
	namespace
	{
		auto ValidateCommonInput(
			FArchive& Ar,
			std::string_view ImporterId,
			uint32 ImporterVersion,
			const FStaticMeshImportSettings& ImportSettings,
			EStaticMeshTargetPlatform TargetPlatform) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"StaticMesh build-key input is save-only.");
				return false;
			}
			std::string Error;
			if (ImporterId.empty() || ImporterVersion == 0)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh importer identity is incomplete.");
				return false;
			}
			if (!ImportSettings.IsValid(&Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
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
			std::string& OutError) -> std::vector<std::byte>
		{
			std::vector<std::byte> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
			if (Ar.HasError()) Bytes.clear();
			return Bytes;
		}
	}

	auto FStaticMeshBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateCommonInput(
			Ar, ImporterId, ImporterVersion, ImportSettings, TargetPlatform)) return;
		uint32 KeySchemaVersion = StaticMeshDerivedDataKeySchemaVersion;
		uint8 ForwardAxis = static_cast<uint8>(ImportSettings.ForwardAxis);
		uint8 RightAxis = static_cast<uint8>(ImportSettings.RightAxis);
		uint8 UpAxis = static_cast<uint8>(ImportSettings.UpAxis);
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << SourceContentHash.HashLow << SourceContentHash.HashHigh
			<< ReconciliationHash.HashLow << ReconciliationHash.HashHigh
			<< ImporterId << ImporterVersion << ForwardAxis << RightAxis << UpAxis
			<< BuilderVersion << PayloadSchemaVersion << Platform;
	}

	auto FStaticMeshCollisionBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateCommonInput(
			Ar, ImporterId, ImporterVersion, ImportSettings, TargetPlatform)) return;
		uint32 KeySchemaVersion = StaticMeshCollisionKeySchemaVersion;
		uint8 ForwardAxis = static_cast<uint8>(ImportSettings.ForwardAxis);
		uint8 RightAxis = static_cast<uint8>(ImportSettings.RightAxis);
		uint8 UpAxis = static_cast<uint8>(ImportSettings.UpAxis);
		uint8 Mode = static_cast<uint8>(SourceMode);
		uint8 Policy = static_cast<uint8>(QueryPolicy);
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << SourceContentHash.HashLow << SourceContentHash.HashHigh
			<< GeometryHash.HashLow << GeometryHash.HashHigh << ImporterId
			<< ImporterVersion << ForwardAxis << RightAxis << UpAxis << Mode << Policy
			<< WeldToleranceBits << BuilderVersion << PayloadSchemaVersion << Platform;
	}

	auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<std::byte> Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>
	{
		return BuildKeyBytes(Input, OutError);
	}

	auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<std::byte> Bytes = BuildKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
