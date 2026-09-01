#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/Load.h"
#include "Asset/PackageInspection.h"

namespace Durin
{
	namespace FFileHelper { class IFileHandle; }
}

namespace Durin
{
	enum class EPackageSchemaIssueCode : uint8
	{
		UnknownField,
		IncompatibleFieldSignature,
		DeprecatedRouteUsed,
		UnavailableClass,
		InvalidObjectGraph
	};

	enum class EPackageSchemaStatus : uint8 { Compatible, Incompatible, Unsupported };

	struct FReflectionSchemaField
	{
		std::string DeclaringType;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		auto operator==(const FReflectionSchemaField&) const -> bool = default;
	};

	struct FReflectionSchemaClass
	{
		std::string QualifiedName;
		bool bConstructible = false;
		std::vector<std::string> Ancestry;
		std::vector<FReflectionSchemaField> Fields;
		auto operator==(const FReflectionSchemaClass&) const -> bool = default;
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

	// Immutable reflection snapshot used by worker-side schema inspection. Ordinary
	// package loading captures it locally and does not expose maintenance concepts.
	class FReflectionSchemaCatalog
	{
	public:
		ENGINE_API static auto Capture() -> FReflectionSchemaCatalog;
		ENGINE_API auto FindClass(std::string_view QualifiedName) const -> const FReflectionSchemaClass*;
		ENGINE_API auto FindField(const FReflectionSchemaClass& ObjectClass,
			std::string_view DeclaringType, std::string_view Name) const -> const FReflectionSchemaField*;
		auto GetClasses() const -> std::span<const FReflectionSchemaClass> { return Classes; }
		ENGINE_API auto FindSerializedAlias(std::string_view StoredIdentity) const
			-> const FReflectionSerializedAlias*;
		auto GetSerializedAliases() const -> std::span<const FReflectionSerializedAlias> { return SerializedAliases; }
		ENGINE_API auto FindSerializedPropertyAlias(std::string_view DeclaringType,
			std::string_view StoredName) const -> const FReflectionSerializedPropertyAlias*;
		auto GetSerializedPropertyAliases() const -> std::span<const FReflectionSerializedPropertyAlias>
			{ return SerializedPropertyAliases; }
		ENGINE_API auto FindDeprecatedPropertyRoute(std::string_view DeclaringType,
			std::string_view StoredName, DurinCodeGen::EPropertyGenFlags Kind,
			std::string_view TypeSignature,
			std::span<const std::pair<FGuid, int32>> CustomVersions) const
			-> const FReflectionDeprecatedPropertyRoute*;
		auto GetDeprecatedPropertyRoutes() const -> std::span<const FReflectionDeprecatedPropertyRoute>
			{ return DeprecatedPropertyRoutes; }

	private:
		std::vector<FReflectionSchemaClass> Classes;
		std::vector<FReflectionSerializedAlias> SerializedAliases;
		std::vector<FReflectionSerializedPropertyAlias> SerializedPropertyAliases;
		std::vector<FReflectionDeprecatedPropertyRoute> DeprecatedPropertyRoutes;
	};

	struct FPackageSchemaIssue
	{
		EPackageSchemaIssueCode Code = EPackageSchemaIssueCode::InvalidObjectGraph;
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
		auto operator==(const FPackageSchemaIssue&) const -> bool = default;
	};

	struct FPackageSchemaInspection
	{
		uint32 FormatVersion = 0;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::vector<FPackagePath> Dependencies;
		EPackageSchemaStatus Status = EPackageSchemaStatus::Compatible;
		std::vector<FPackageSchemaIssue> Issues;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;
	};

	struct FPackageSchemaReadStats
	{
		uint64 MetadataBytesRead = 0;
		uint64 PayloadBytesSkipped = 0;
		uint64 PeakMetadataBytes = 0;
	};

	using FPackageReadCancellationCheck = std::function<bool()>;

	// Reads only the current package format's schema/value descriptors unless nested
	// migration evidence is explicitly requested. The caller retains handle ownership.
	ENGINE_API auto InspectAssetPackageSchema(FFileHelper::IFileHandle& Handle, const FPackagePath& PackagePath,
		const FReflectionSchemaCatalog& Catalog, FPackageSchemaInspection& OutInspection,
		FPackageSchemaReadStats* OutStats = nullptr,
		bool bIncludeNestedMigrationEvidence = false,
		const FPackageReadCancellationCheck& IsCancellationRequested = {}) -> FAssetResult;
}
