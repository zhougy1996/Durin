#pragma once

#include "AssetForge/Extensions/ComponentRegistration.h"

namespace Durin::AssetForge
{
	struct FOutputMapping
	{
		std::string SourceNodeIdentity;
		std::string OutputIdentity;
		FAssetPath AssetPath;

		auto operator==(const FOutputMapping&) const -> bool = default;
	};

	// Framework-owned reproduction identity persisted by both single and
	// multi-output imports.
	struct FImportProvenance
	{
		uint32 SchemaVersion = AssetForgeContractVersion;
		FComponentIdentity Translator;
		std::vector<FPlanningPassStackEntry> PlanningPassStack;
		struct FSourceProvenance
		{
			std::string StableIdentity;
			std::string Role;
			FSourcePath SourcePath;
			FXxHash128 ContentHash{};
			uint64 ByteCount = 0;
			auto operator==(const FSourceProvenance&) const -> bool = default;
		};
		std::vector<FSourceProvenance> Sources;
		std::vector<FOutputMapping> OutputMappings;
		FXxHash128 SourceGraphFingerprint{};
		FXxHash128 BuildGraphFingerprint{};
		std::string AuthoredOutputFingerprint;

		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FImportProvenance&) const -> bool = default;
	};

	ASSETFORGE_API auto SerializeImportProvenance(
		const FImportProvenance& Provenance,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto DeserializeImportProvenance(
		std::span<const std::byte> Bytes,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool;
}
