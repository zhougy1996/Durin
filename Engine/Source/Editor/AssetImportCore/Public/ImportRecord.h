#pragma once

#include "Asset/Result.h"
#include "Asset/SourcePath.h"
#include "AssetImportCoreAPI.h"
#include "DObject/AssetPath.h"
#include "DObject/CoreDObject.h"

#include "ImportRecord.gen.h"

namespace Durin::Asset
{
	struct FAssetResult;
}

namespace Durin::Asset
{
	class DImportRecord;
	ASSETIMPORTCORE_API auto CreateImportRecordAsset(
		const FAssetPath& Path,
		DImportRecord*& OutRecord
	) -> Asset::FAssetResult;

	inline constexpr uint32 ImportRecordVersion = 2;
	inline constexpr uint32 MinimumSupportedImportRecordVersion = ImportRecordVersion;
	inline constexpr uint64 MaximumImportRecordSettingsBytes = 4ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumImportRecordProviderStateBytes = 16ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumImportRecordSources = 8192;
	inline constexpr uint32 MaximumImportRecordOutputs = 8192;
	inline constexpr uint32 MaximumImportRecordDetachedTombstones = 1024;
	inline constexpr uint32 MaximumImportRecordAcceptedDiagnostics = 1024;

	struct FImportRecordSerializationVersion
	{
		inline static constexpr FGuid Guid{
			0x86b18d5e, 0xd75c42c2, 0x9ad23d7f, 0xa6901f16};
		enum Type : int32
		{
			BeforeCustomVersionWasAdded = -1,
			BytePayloadBlob = 1,
			LatestVersion = BytePayloadBlob,
		};
	};

	DSTRUCT()
	struct FImportRecordPayload
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string SchemaId;

		DPROPERTY()
		uint32 SchemaVersion = 0;

		DPROPERTY()
		std::vector<std::byte> Bytes;

		DPROPERTY(Deprecated,
			CustomVersion = Durin::Asset::FImportRecordSerializationVersion,
			DeprecatedBefore = Durin::Asset::FImportRecordSerializationVersion::BytePayloadBlob,
			DeprecatedName = "Bytes",
			MigratesTo = "Bytes")
		std::vector<uint8> Bytes_DEPRECATED;

		DPROPERTY()
		uint64 ContentHashLow = 0;

		DPROPERTY()
		uint64 ContentHashHigh = 0;

		auto operator==(const FImportRecordPayload&) const -> bool = default;
	};

	DSTRUCT()
	struct FImportRecordSource
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string StableIdentity;

		DPROPERTY()
		std::string Role;

		DPROPERTY()
		FSourcePath SourcePath;

		DPROPERTY()
		uint64 ContentHashLow = 0;

		DPROPERTY()
		uint64 ContentHashHigh = 0;

		DPROPERTY()
		uint64 ByteCount = 0;

		auto operator==(const FImportRecordSource&) const -> bool = default;
	};

	DENUM()
	enum class EImportRecordOutputPolicy : uint8
	{
		Managed,
		Referenced,
		Detached
	};

	DSTRUCT()
	struct FImportRecordOutput
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string StableIdentity;

		DPROPERTY()
		std::string Role;

		FAssetPath AssetPath;

		DPROPERTY()
		std::string AssetPathText;

		DPROPERTY()
		std::string AssetClassName;

		DPROPERTY()
		EImportRecordOutputPolicy Policy = EImportRecordOutputPolicy::Managed;

		DPROPERTY()
		std::string AuthoredFingerprint;

		auto operator==(const FImportRecordOutput&) const -> bool = default;
	};

	DSTRUCT()
	struct FImportRecordDetachedTombstone
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string StableIdentity;

		FAssetPath LastAssetPath;

		DPROPERTY()
		std::string LastAssetPathText;

		DPROPERTY()
		std::string LastAuthoredFingerprint;

		DPROPERTY()
		uint64 Sequence = 0;

		auto operator==(const FImportRecordDetachedTombstone&) const -> bool = default;
	};

	DSTRUCT()
	struct FImportRecordDiagnostic
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string Identity;

		DPROPERTY()
		uint8 Severity = 0;

		DPROPERTY()
		uint8 Category = 0;

		DPROPERTY()
		std::string Phase;

		DPROPERTY()
		std::string SourceIdentity;

		DPROPERTY()
		std::string OutputIdentity;

		DPROPERTY()
		std::string Message;

		auto operator==(const FImportRecordDiagnostic&) const -> bool = default;
	};

	struct FImportRecordState
	{
		std::string ProviderId;
		uint32 ProviderContractVersion = 0;
		FImportRecordPayload Settings;
		FImportRecordPayload ProviderState;
		std::vector<FImportRecordSource> Sources;
		std::vector<FImportRecordOutput> Outputs;
		std::vector<FImportRecordDetachedTombstone> DetachedTombstones;
		std::vector<FImportRecordDiagnostic> AcceptedDiagnostics;
		FAssetPath PrimaryOutput;

		auto operator==(const FImportRecordState&) const -> bool = default;
	};

	DCLASS()
	class DImportRecord : public DObject
	{
		GENERATED_BODY()
	public:
		ASSETIMPORTCORE_API explicit DImportRecord(
			const FObjectInitializer& ObjectInitializer);

		auto GetRecordVersion() const -> uint32 { return RecordVersion; }
		auto GetRecordId() const -> const FGuid& { return RecordId; }
		auto GetProviderId() const -> std::string_view { return ProviderId; }
		auto GetProviderContractVersion() const -> uint32 { return ProviderContractVersion; }
		auto GetSettings() const -> const FImportRecordPayload& { return Settings; }
		auto GetProviderState() const -> const FImportRecordPayload& { return ProviderState; }
		auto GetSources() const -> std::span<const FImportRecordSource> { return Sources; }
		auto GetOutputs() const -> std::span<const FImportRecordOutput> { return Outputs; }
		auto GetDetachedTombstones() const
			-> std::span<const FImportRecordDetachedTombstone> { return DetachedTombstones; }
		auto GetAcceptedDiagnostics() const
			-> std::span<const FImportRecordDiagnostic> { return AcceptedDiagnostics; }
		auto GetPrimaryOutput() const -> const FAssetPath& { return PrimaryOutput; }
		auto IsCookExcluded() const -> bool { return bExcludedFromCook; }

		ASSETIMPORTCORE_API auto GetState() const -> FImportRecordState;
		ASSETIMPORTCORE_API auto SetState(FImportRecordState State, std::string& OutError) -> bool;
		ASSETIMPORTCORE_API auto SetRecordIdForClone(const FGuid& NewId, std::string& OutError) -> bool;
		ASSETIMPORTCORE_API auto ReplaceOutputPath(
			const FAssetPath& OldPath,
			const FAssetPath& NewPath) -> bool;
		ASSETIMPORTCORE_API auto Validate(std::string& OutError) const -> bool;
		ASSETIMPORTCORE_API auto GetFingerprint() const -> std::string;
		ASSETIMPORTCORE_API auto ExchangeImportedState(DImportRecord& Other) noexcept -> void;
		ASSETIMPORTCORE_API auto PostLoad(std::string& OutError) -> bool override;

	private:
		friend ASSETIMPORTCORE_API auto CreateImportRecordAsset(
			const FAssetPath& Path,
			DImportRecord*& OutRecord
		) -> Asset::FAssetResult;

		DPROPERTY()
		uint32 RecordVersion = ImportRecordVersion;

		DPROPERTY()
		FGuid RecordId;

		DPROPERTY()
		std::string ProviderId;

		DPROPERTY()
		uint32 ProviderContractVersion = 0;

		DPROPERTY()
		FImportRecordPayload Settings;

		DPROPERTY()
		FImportRecordPayload ProviderState;

		DPROPERTY()
		std::vector<FImportRecordSource> Sources;

		DPROPERTY()
		std::vector<FImportRecordOutput> Outputs;

		DPROPERTY()
		std::vector<FImportRecordDetachedTombstone> DetachedTombstones;

		DPROPERTY()
		std::vector<FImportRecordDiagnostic> AcceptedDiagnostics;

		FAssetPath PrimaryOutput;

		DPROPERTY()
		std::string PrimaryOutputPathText;

		DPROPERTY()
		bool bExcludedFromCook = true;
	};

	ASSETIMPORTCORE_API auto MakeImportRecordPayload(
		std::string SchemaId,
		uint32 SchemaVersion,
		std::span<const std::byte> Bytes,
		uint64 MaximumBytes,
		FImportRecordPayload& OutPayload,
		std::string& OutError) -> bool;
	ASSETIMPORTCORE_API auto MakeSiblingImportRecordPath(
		const FAssetPath& SiblingOutput,
		std::string_view SourceName,
		FAssetPath& OutPath,
		std::string& OutError) -> bool;
}

namespace Durin
{
	template<>
	struct TDStructOpsTraits<Asset::FImportRecordPayload>
		: TDStructOpsTraitsBase<Asset::FImportRecordPayload>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(
			Asset::FImportRecordPayload& Value,
			FDStructPostDeserializeContext& Context) -> bool
		{
			const FArchiveCustomVersion* Version = Context.VersionContext
				? Context.VersionContext->FindCustom(
					Asset::FImportRecordSerializationVersion::Guid)
				: nullptr;
			if ((!Version
					? Asset::FImportRecordSerializationVersion::BeforeCustomVersionWasAdded
					: Version->Version)
				< Asset::FImportRecordSerializationVersion::BytePayloadBlob
				&& !Value.Bytes_DEPRECATED.empty())
			{
				Value.Bytes.resize(Value.Bytes_DEPRECATED.size());
				std::ranges::transform(Value.Bytes_DEPRECATED, Value.Bytes.begin(),
					[](uint8 Byte) { return static_cast<std::byte>(Byte); });
				Value.Bytes_DEPRECATED.clear();
			}
			return true;
		}
	};

	template<>
	struct TDStructOpsTraits<Asset::FImportRecordOutput>
		: TDStructOpsTraitsBase<Asset::FImportRecordOutput>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(
			Asset::FImportRecordOutput& Value,
			FDStructPostDeserializeContext& Context) -> bool
		{
			std::string Error;
			if (FAssetPath::TryCreate(Value.AssetPathText, Value.AssetPath, &Error))
				return true;
			return Context.Fail(Error);
		}
	};

	template<>
	struct TDStructOpsTraits<Asset::FImportRecordDetachedTombstone>
		: TDStructOpsTraitsBase<Asset::FImportRecordDetachedTombstone>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(
			Asset::FImportRecordDetachedTombstone& Value,
			FDStructPostDeserializeContext& Context) -> bool
		{
			std::string Error;
			if (FAssetPath::TryCreate(
				Value.LastAssetPathText, Value.LastAssetPath, &Error))
				return true;
			return Context.Fail(Error);
		}
	};
}
