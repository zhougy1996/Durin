#pragma once

#include "AssetForgeAPI.h"
#include "Asset/AssetImportData.h"

#include "AssetForgeImportData.gen.h"

namespace Durin::AssetForge
{
	inline constexpr uint32 AssetForgeImportDataSchemaVersion = 1;
	inline constexpr uint32 MaximumAssetImportPlanningPasses = 1'024;
	inline constexpr uint32 MaximumAssetImportOutputMappings = 8'192;
	inline constexpr uint64 MaximumAssetImportSettingsBytes = 4ull * 1'024ull * 1'024ull;
	inline constexpr uint64 MaximumAssetImportPayloadBytes = 16ull * 1'024ull * 1'024ull;

	DSTRUCT()
	struct FAssetImportPayload
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string SchemaId;

		DPROPERTY()
		uint32 SchemaVersion = 0;

		DPROPERTY()
		std::vector<std::byte> Bytes;

		DPROPERTY()
		uint64 ContentHashLow = 0;

		DPROPERTY()
		uint64 ContentHashHigh = 0;

		ASSETFORGE_API auto IsEmpty() const -> bool;
		ASSETFORGE_API auto Validate(
			uint64 MaximumBytes, std::string& OutError) const -> bool;
		auto operator==(const FAssetImportPayload&) const -> bool = default;
	};

	DSTRUCT()
	struct FAssetImportComponentDescriptor
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string ComponentId;

		DPROPERTY()
		uint32 ContractVersion = 0;

		DPROPERTY()
		FAssetImportPayload Settings;

		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FAssetImportComponentDescriptor&) const -> bool = default;
	};

	DSTRUCT()
	struct FAssetImportPlanningPassDescriptor
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string PlanningPassId;

		DPROPERTY()
		uint32 ContractVersion = 0;

		DPROPERTY()
		FAssetImportPayload Settings;

		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FAssetImportPlanningPassDescriptor&) const -> bool = default;
	};

	DSTRUCT()
	struct FAssetImportSourceReference
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string StableIdentity;

		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FAssetImportSourceReference&) const -> bool = default;
	};

	DSTRUCT()
	struct FAssetImportOutputMapping
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string SourceNodeIdentity;

		DPROPERTY()
		std::string OutputIdentity;

		DPROPERTY()
		std::string AssetPathText;

		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FAssetImportOutputMapping&) const -> bool = default;
	};

	struct FAssetForgeImportState : AssetImport::FAssetImportDataState
	{
		uint32 ReplaySchemaVersion = AssetForgeImportDataSchemaVersion;
		FAssetImportComponentDescriptor Translator;
		std::vector<FAssetImportPlanningPassDescriptor> PlanningPassStack;
		std::vector<FAssetImportSourceReference> SourceReferences;
		std::vector<FAssetImportOutputMapping> OutputMappings;
		uint64 SourceGraphFingerprintLow = 0;
		uint64 SourceGraphFingerprintHigh = 0;
		uint64 BuildGraphFingerprintLow = 0;
		uint64 BuildGraphFingerprintHigh = 0;
		std::string AuthoredOutputFingerprint;

		auto operator==(const FAssetForgeImportState&) const -> bool = default;
	};

	DCLASS()
	class DAssetForgeImportData final : public AssetImport::DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGE_API explicit DAssetForgeImportData(
			const FObjectInitializer& ObjectInitializer);

		auto GetTranslator() const -> const FAssetImportComponentDescriptor&
		{
			return Translator;
		}
		auto GetPlanningPassStack() const
			-> std::span<const FAssetImportPlanningPassDescriptor>
		{
			return PlanningPassStack;
		}
		auto GetSourceReferences() const
			-> std::span<const FAssetImportSourceReference>
		{
			return SourceReferences;
		}
		auto GetOutputMappings() const -> std::span<const FAssetImportOutputMapping>
		{
			return OutputMappings;
		}

		ASSETFORGE_API auto SetState(
			FAssetForgeImportState State, std::string& OutError) -> bool;
		ASSETFORGE_API auto GetAssetForgeState() const -> FAssetForgeImportState;
		auto GetState() const -> AssetImport::FAssetImportDataState override
		{
			return GetAssetForgeState();
		}
		ASSETFORGE_API auto Validate(std::string& OutError) const -> bool override;
		ASSETFORGE_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> AssetImport::DAssetImportData* override;

	private:
		DPROPERTY()
		uint32 ReplaySchemaVersion = AssetForgeImportDataSchemaVersion;

		DPROPERTY()
		FAssetImportComponentDescriptor Translator;

		DPROPERTY()
		std::vector<FAssetImportPlanningPassDescriptor> PlanningPassStack;

		DPROPERTY()
		std::vector<FAssetImportSourceReference> SourceReferences;

		DPROPERTY()
		std::vector<FAssetImportOutputMapping> OutputMappings;

		DPROPERTY()
		uint64 SourceGraphFingerprintLow = 0;

		DPROPERTY()
		uint64 SourceGraphFingerprintHigh = 0;

		DPROPERTY()
		uint64 BuildGraphFingerprintLow = 0;

		DPROPERTY()
		uint64 BuildGraphFingerprintHigh = 0;

		DPROPERTY()
		std::string AuthoredOutputFingerprint;
	};

	ASSETFORGE_API auto MakeAssetImportPayload(
		std::string SchemaId,
		uint32 SchemaVersion,
		std::span<const std::byte> Bytes,
		uint64 MaximumBytes,
		FAssetImportPayload& OutPayload,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto ValidateAssetImportDataState(
		const FAssetForgeImportState& State,
		std::string& OutError) -> bool;
}
