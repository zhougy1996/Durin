#pragma once

#include "EngineAPI.h"
#include "Asset/Load.h"
#include "Asset/PackageInspection.h"

namespace Durin::Asset
{
	inline constexpr uint32 AssetCompatibilityReportSchemaVersion = 3;

	// Stable names are serialized by report schemas; enum ordinals are never persisted.
	enum class EAssetCompatibilityFindingCode : uint8
	{
		UnknownField,
		IncompatibleFieldSignature,
		DeprecatedRouteUsed,
		UnavailableClass,
		UnsupportedPackageFormat,
		InvalidObjectGraph,
		CorruptPackage,
		IoFailure
	};

	enum class EAssetCompatibilityInspection : uint8 { NotChecked, Ready, Failed };
	enum class EAssetPackageCompatibility : uint8 { Compatible, Incompatible, Unsupported };
	enum class EAssetCompatibilityFreshness : uint8 { Current, Stale };
	enum class EAssetCompatibilityProbeStatus : uint8 { Completed, Cancelled };

	struct FReflectionCompatibilityField
	{
		std::string DeclaringType;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;

		auto operator==(const FReflectionCompatibilityField&) const -> bool = default;
	};

	struct FReflectionCompatibilityClass
	{
		std::string QualifiedName;
		bool bConstructible = false;
		std::vector<std::string> Ancestry;
		std::vector<FReflectionCompatibilityField> Fields;

		auto operator==(const FReflectionCompatibilityClass&) const -> bool = default;
	};

	struct FReflectionSerializedAlias
	{
		std::string StoredIdentity;
		std::string CurrentIdentity;
		EAssetReflectedIdentityKind Kind = EAssetReflectedIdentityKind::Class;

		auto operator==(const FReflectionSerializedAlias&) const -> bool = default;
	};

	struct FReflectionSerializedPropertyAlias
	{
		std::string DeclaringType;
		std::string StoredName;
		std::string CurrentName;

		auto operator==(const FReflectionSerializedPropertyAlias&) const -> bool = default;
	};

	struct FReflectionDeprecatedPropertyRoute
	{
		std::string DeclaringType;
		std::string DeprecatedPropertyName;
		std::string StoredName;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		FGuid CustomVersionGuid;
		int32 DeprecatedBefore = 0;
		int32 LatestVersion = 0;
		std::vector<std::string> MigrationTargets;

		auto operator==(const FReflectionDeprecatedPropertyRoute&) const -> bool = default;
	};

	// Value-only reflection snapshot. Capture on the game thread after type registration,
	// then copy or share it freely with compatibility workers.
	class FReflectionCompatibilityCatalog
	{
	public:
		ENGINE_API static auto Capture() -> FReflectionCompatibilityCatalog;
		ENGINE_API auto FindClass(std::string_view QualifiedName) const -> const FReflectionCompatibilityClass*;
		ENGINE_API auto FindField(
			const FReflectionCompatibilityClass& ObjectClass,
			std::string_view DeclaringType,
			std::string_view Name) const -> const FReflectionCompatibilityField*;
		auto GetClasses() const -> std::span<const FReflectionCompatibilityClass> { return Classes; }
		ENGINE_API auto FindSerializedAlias(std::string_view StoredIdentity) const
			-> const FReflectionSerializedAlias*;
		auto GetSerializedAliases() const -> std::span<const FReflectionSerializedAlias> { return SerializedAliases; }
		ENGINE_API auto FindSerializedPropertyAlias(
			std::string_view DeclaringType, std::string_view StoredName) const
			-> const FReflectionSerializedPropertyAlias*;
		auto GetSerializedPropertyAliases() const
			-> std::span<const FReflectionSerializedPropertyAlias> { return SerializedPropertyAliases; }
		ENGINE_API auto FindDeprecatedPropertyRoute(
			std::string_view DeclaringType, std::string_view StoredName,
			DurinCodeGen::EPropertyGenFlags Kind, std::string_view TypeSignature,
			std::span<const std::pair<FGuid, int32>> CustomVersions) const
			-> const FReflectionDeprecatedPropertyRoute*;
		auto GetDeprecatedPropertyRoutes() const
			-> std::span<const FReflectionDeprecatedPropertyRoute> { return DeprecatedPropertyRoutes; }

	private:
		std::vector<FReflectionCompatibilityClass> Classes;
		std::vector<FReflectionSerializedAlias> SerializedAliases;
		std::vector<FReflectionSerializedPropertyAlias> SerializedPropertyAliases;
		std::vector<FReflectionDeprecatedPropertyRoute> DeprecatedPropertyRoutes;
	};

	struct FAssetCompatibilityFinding
	{
		EAssetCompatibilityFindingCode Code = EAssetCompatibilityFindingCode::CorruptPackage;
		std::string ObjectPath;
		std::string ClassIdentity;
		std::string DeclaringType;
		std::string FieldName;
		DurinCodeGen::EPropertyGenFlags StoredKind = DurinCodeGen::EPropertyGenFlags::None;
		std::string StoredTypeSignature;
		DurinCodeGen::EPropertyGenFlags ExpectedKind = DurinCodeGen::EPropertyGenFlags::None;
		std::string ExpectedTypeSignature;
		uint64 PayloadSize = 0;
		uint64 PayloadOffset = 0;
		std::string Diagnostic;

		auto operator==(const FAssetCompatibilityFinding&) const -> bool = default;
	};

	struct FAssetPackageCompatibilityRecord
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		FAssetPackageFingerprint Fingerprint;
		std::string ReportContentHash;
		uint32 FormatVersion = 0;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::vector<FAssetPath> Dependencies;
		EAssetCompatibilityInspection Inspection = EAssetCompatibilityInspection::NotChecked;
		EAssetPackageCompatibility Compatibility = EAssetPackageCompatibility::Unsupported;
		EAssetCompatibilityFreshness Freshness = EAssetCompatibilityFreshness::Current;
		std::vector<FAssetCompatibilityFinding> Findings;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;

		auto operator==(const FAssetPackageCompatibilityRecord&) const -> bool = default;
	};

	struct FAssetCompatibilityProbeStats
	{
		uint64 MetadataBytesRead = 0;
		uint64 PayloadBytesSkipped = 0;
		uint64 PeakMetadataBytes = 0;
	};

	struct FAssetPackageCompatibilityProbeInput
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		uintmax_t ExpectedFileSize = 0;
		int64 ExpectedLastWriteTimeTicks = 0;
		FXxHash128 ExpectedContentHash;
		std::string ExpectedReportContentHash;
	};

	enum class EAssetPackageSnapshotStatus : uint8 { Completed, Cancelled, Failed };

	struct FAssetPackageDiscoverySnapshot
	{
		EAssetPackageSnapshotStatus Status = EAssetPackageSnapshotStatus::Completed;
		std::vector<FAssetPackageCompatibilityProbeInput> Packages;
		std::string Error;
	};

	struct FAssetPackageCompatibilityProbeResult
	{
		EAssetCompatibilityProbeStatus Status = EAssetCompatibilityProbeStatus::Completed;
		std::optional<FAssetPackageCompatibilityRecord> Record;
		FAssetCompatibilityProbeStats Stats;
	};

	using FAssetCompatibilityCancellationCheck = std::function<bool()>;

	// Captures auto-scan mounts without constructing assets or publishing registry state.
	ENGINE_API auto CaptureMountedAssetPackageSnapshot(
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageDiscoverySnapshot;

	ENGINE_API auto ProbeAssetPackageCompatibility(
		const FAssetPackageCompatibilityProbeInput& Input,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested = {})
		-> FAssetPackageCompatibilityProbeResult;

	ENGINE_API auto IsAssetPackageCompatibilityRecordCurrent(
		const FAssetPackageCompatibilityRecord& Record,
		uintmax_t FileSize,
		int64 LastWriteTimeTicks) -> bool;

	ENGINE_API auto AssetCompatibilityFindingCodeName(EAssetCompatibilityFindingCode Code) -> std::string_view;
	ENGINE_API auto SerializeAssetCompatibilityReportV1(
		std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string;
}
