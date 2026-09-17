#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/CanonicalMapKey.h"
#include "Serialization/BinaryEnvelope.h"
#include "Hash/XxHash.h"

namespace Durin::ObjectPackage
{
	inline constexpr uint32 DastPackageMagic = 0x54534144;
	inline constexpr FGuid DastFormatId{
		0x3c59d1a9, 0x6ceb4e4c, 0xb059452d, 0xb0a5af56};
	inline constexpr std::string_view DastFormatName = "Durin.BinaryFormat.DAST";
	inline constexpr uint32 DastV10FormatVersion = 10;
	inline constexpr std::array SupportedPackageReaderVersions{DastV10FormatVersion};
	// Persisted projections use a policy generation so supported-reader sets cannot alias.
	inline constexpr uint32 PackageReaderPolicyFingerprint = 0x4150430b;

	constexpr auto IsSupportedPackageReaderVersion(uint32 Version) -> bool
	{
		return std::ranges::find(SupportedPackageReaderVersions, Version)
			!= SupportedPackageReaderVersions.end();
	}

	inline constexpr uint32 DastRegistryVersion = 2;
	inline constexpr uint32 DastTableVersion = 1;
	inline constexpr uint32 DastFormatHeaderBytes = 32;
	inline constexpr uint32 DastSectionEntryBytes = 48;
	inline constexpr uint32 DastSectionCount = 9;
	inline constexpr uint64 DastFormatHeaderOffset = 64;
	inline constexpr uint64 DastDirectoryOffset = 96;
	inline constexpr uint64 DastFirstSectionOffset = 528;
	inline constexpr uint64 DastMaximumHeaderBytes = 16ull * 1024ull * 1024ull;
	inline constexpr uint64 DastMaximumPackageBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 DastMaximumBulkBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 DastMaximumTableEntries = 1'048'575;
	inline constexpr uint64 DastMaximumStringBytes = 1024ull * 1024ull;
	inline constexpr uint64 DastMaximumContainerElements = 1'048'575;
	inline constexpr uint32 DastMaximumValueDepth = 64;

	enum class EDastSection : uint32
	{
		Registry = 1,
		Names = 2,
		Imports = 3,
		Exports = 4,
		Types = 5,
		Schemas = 6,
		Values = 7,
		BulkDirectory = 8,
		InlineBulk = 9,
	};

	enum class EPackageWriterFailure : uint8
	{
		None,
		InvalidInput,
		InvalidUtf8,
		InvalidIndex,
		InvalidTopology,
		InvalidType,
		InvalidValue,
		DuplicateIdentity,
		ManifestMismatch,
		InvalidBulkData,
		LimitExceeded,
		ArithmeticOverflow,
		EnvelopeFailure,
		AliasedOutput,
	};

	enum class EPackageWriterReason : uint8
	{
		None,
		RequiredNameEmpty,
		NameLimit,
		InvalidNameUtf8,
		TypeDepth,
		InvalidEnumType,
		InvalidIntrinsicType,
		MissingStructName,
		InvalidFixedArrayType,
		InvalidArrayType,
		InvalidMapType,
		ScalarChildren,
		UnresolvedTablePath,
		ImportIndex,
		ExportIndex,
		InvalidStructBaseline,
		ValueDepth,
		SignedRange,
		UnsignedRange,
		Float32Width,
		EnumRange,
		InvalidString,
		IntrinsicComponentCount,
		MissingStructSchema,
		MissingHardReferenceFields,
		MissingCompleteFields,
		StructDescriptor,
		DuplicateStructField,
		StructFieldType,
		FixedArrayCount,
		ArrayLimit,
		MapCount,
		DuplicateMapKey,
		BlobLimit,
		InvalidBulkDescriptor,
		ReferenceDepth,
		InvalidSoftPath,
		UnsupportedVersion,
		TableLimit,
		InvalidTopLevelAsset,
		TopLevelExportMismatch,
		DuplicateImport,
		DuplicateExport,
		DuplicateTopLevelAsset,
		MissingTopLevelAsset,
		DuplicateSchemaField,
		DuplicateSchema,
		TypeTableLimit,
		CustomVersionLimit,
		InvalidCustomVersion,
		DuplicateCustomVersion,
		OpaqueProperty,
		DuplicateProperty,
		MissingPropertySchema,
		PropertySchemaMismatch,
		NameTableLimit,
		EmissionDepth,
		ByteRange,
		BulkOrder,
		ValueSectionLimit,
		BulkAlignmentOverflow,
		BulkPaddingLimit,
		BulkSegmentLimit,
		MissingBulkPayload,
		RecordLimit,
		BulkBindingMismatch,
		SectionLimit,
		BulkCount,
		PackageLimit,
		HeaderLimit,
		AliasedOutput,
		InvalidBulkBinding,
		EnvelopeRejected,
		CanonicalKeyRejected,
		IncompleteEncoding,
	};

	struct FPackageWriterResult
	{
		EPackageWriterFailure Failure = EPackageWriterFailure::None;
		std::string LogicalPath;
		EPackageWriterReason Reason = EPackageWriterReason::None;
		std::variant<std::monostate, EBinaryEnvelopeError, FLinkerError, FCanonicalMapKeyError> Cause;
		std::string Subject;

		auto Succeeded() const -> bool { return Failure == EPackageWriterFailure::None; }
		explicit operator bool() const { return Succeeded(); }
		auto Reset() -> void { *this = {}; }
	};
	COREDOBJECT_API auto FormatPackageError(const FPackageWriterResult& Error) -> std::string;


	struct FPackageWriterManifest
	{
		std::vector<std::string> Names;
		std::vector<FSerializedType> Types;
		std::vector<std::string> Schemas;
		std::vector<std::string> Imports;
		std::vector<std::string> Exports;
		std::vector<std::string> BulkValues;

		auto operator==(const FPackageWriterManifest&) const -> bool = default;
	};

	struct FPackageAssetRegistryData
	{
		uint32 ExportId = 0;
		FTopLevelAssetPath AssetPath;
		std::string ClassName;
		FObjectPath RedirectDestination;

		auto operator==(const FPackageAssetRegistryData&) const -> bool = default;
	};

	struct FPackageRegistryData
	{
		FPackagePath PackagePath;
		uint32 ExportCount = 0;
		std::vector<FPackageAssetRegistryData> TopLevelAssets;
		std::vector<FPackagePath> HardPackageReferences;
		std::vector<FPackagePath> SoftPackageReferences;
		std::vector<std::string> SearchableNames;
		uint64 ExternalBulkBytes = 0;
		FXxHash128 ExternalBulkHash;

		auto operator==(const FPackageRegistryData&) const -> bool = default;
	};

	struct FPackageReaderLimits
	{
		uint64 MaximumHeaderBytes = DastMaximumHeaderBytes;
		uint64 MaximumPackageBytes = DastMaximumPackageBytes;
		uint64 MaximumBulkBytes = DastMaximumBulkBytes;
		uint64 MaximumTableEntries = DastMaximumTableEntries;
		uint64 MaximumStringBytes = DastMaximumStringBytes;
		uint64 MaximumContainerElements = DastMaximumContainerElements;
		uint32 MaximumValueDepth = DastMaximumValueDepth;
	};

	enum class EPackageReaderFailure : uint8
	{
		None,
		InvalidEnvelope,
		InvalidFormatHeader,
		InvalidDirectory,
		HashMismatch,
		InvalidRegistry,
		InvalidTable,
		InvalidIndex,
		InvalidTopology,
		InvalidType,
		InvalidValue,
		InvalidBulkData,
		NonCanonical,
		LimitExceeded,
		ArithmeticOverflow,
	};

	enum class EPackageReaderReason : uint8
	{
		None,
		InvalidLimits,
		InputExtent,
		DeclaredExtent,
		UnsupportedVersion,
		FormatHeader,
		Directory,
		SectionOverflow,
		SectionHash,
		SectionCoverage,
		NameTableHeader,
		NonCanonicalNames,
		NameTableTrailing,
		ImportTableHeader,
		ImportRecord,
		ImportPath,
		ImportTableTrailing,
		MissingPackageIdentity,
		RegistryIdentity,
		TopLevelRecord,
		TopLevelPath,
		TopLevelOrder,
		RedirectPath,
		RegistryListCount,
		RegistryListOrder,
		DependencyPath,
		RegistryBulk,
		ExportTableHeader,
		ExportRecord,
		ExportTableTrailing,
		TypeTableHeader,
		TypeRecordExtent,
		TypeRecordHeader,
		ChildTypeIndex,
		TypeRecordTrailing,
		TypeTableTrailing,
		TypeCycle,
		SchemaSectionHeader,
		CustomVersionRecord,
		SchemaCount,
		SchemaHeader,
		SchemaField,
		SchemaTrailing,
		ValueDepth,
		ValueTag,
		StructBaseline,
		ExportBaseline,
		PropertyValue,
		ValueTrailing,
		BulkHandle,
		BulkOwner,
		BulkRange,
		BulkHash,
		BulkLimit,
		MalformedTable,
		TopLevelExportIndex,
		TopLevelTopology,
		TopLevelIndexOverflow,
		MissingTopLevelAsset,
		ExternalBulkHash,
		ExportTopology,
		BulkCoverage,
		CanonicalWriterRejected,
		NonCanonicalBytes,
		EnvelopeRejected,
	};

	struct FPackageReaderResult
	{
		EPackageReaderFailure Failure = EPackageReaderFailure::None;
		std::string LogicalPath;
		EPackageReaderReason Reason = EPackageReaderReason::None;
		std::variant<std::monostate, EBinaryEnvelopeError, FLinkerError, FPackageWriterResult> Cause;
		std::string Subject;

		auto Succeeded() const -> bool { return Failure == EPackageReaderFailure::None; }
		explicit operator bool() const { return Succeeded(); }
		auto Reset() -> void { *this = {}; }
	};
	COREDOBJECT_API auto FormatPackageError(const FPackageReaderResult& Error) -> std::string;


	// Freezes a detached linker model without emitting package bytes.
	COREDOBJECT_API auto FreezePackage(
		const FLinkerTables& Linker,
		FPackageWriterManifest& OutManifest) -> FPackageWriterResult;
	COREDOBJECT_API auto WritePackage(
		const FLinkerTables& Linker,
		FByteBuffer& OutPackageBytes,
		FByteBuffer& OutBulkBytes) -> FPackageWriterResult;
	COREDOBJECT_API auto WritePackageMain(
		const FLinkerTables& Linker,
		uint64 ExternalBulkBytes,
		FXxHash128 ExternalBulkHash,
		FByteBuffer& OutPackageBytes) -> FPackageWriterResult;

	// Validates exactly the declared front matter and publishes package-level Registry data.
	COREDOBJECT_API auto ReadPackageRegistry(
		FByteView FrontMatter,
		uint64 PhysicalPackageBytes,
		uint64 PhysicalBulkBytes,
		const FPackagePath& PackagePath,
		FPackageRegistryData& OutRegistry,
		const FPackageReaderLimits& Limits = {}) -> FPackageReaderResult;
	COREDOBJECT_API auto ReadPackage(
		FByteView PackageBytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FLinkerTables& OutLinker,
		const FPackageReaderLimits& Limits = {}) -> FPackageReaderResult;
	COREDOBJECT_API auto ReadPackageMetadata(
		FByteView PackageBytes,
		uint64 PhysicalBulkBytes,
		const FPackagePath& PackagePath,
		FLinkerTables& OutLinker,
		const FPackageReaderLimits& Limits = {}) -> FPackageReaderResult;
}
