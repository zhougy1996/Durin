#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/PackageLinker.h"
#include "Hash/XxHash.h"

namespace Durin::ObjectPackage
{
	inline constexpr FGuid DastFormatId{
		0x3c59d1a9, 0x6ceb4e4c, 0xb059452d, 0xb0a5af56};
	inline constexpr std::string_view DastFormatName = "Durin.BinaryFormat.DAST";
	inline constexpr uint32 DastV8FormatVersion = 8;
	inline constexpr uint32 DastV8RegistryVersion = 1;
	inline constexpr uint32 DastV8TableVersion = 1;
	inline constexpr uint32 DastV8FormatHeaderBytes = 32;
	inline constexpr uint32 DastV8SectionEntryBytes = 48;
	inline constexpr uint32 DastV8SectionCount = 9;
	inline constexpr uint64 DastV8FormatHeaderOffset = 64;
	inline constexpr uint64 DastV8DirectoryOffset = 96;
	inline constexpr uint64 DastV8FirstSectionOffset = 528;
	inline constexpr uint64 DastV8MaximumHeaderBytes = 16ull * 1024ull * 1024ull;
	inline constexpr uint64 DastV8MaximumPackageBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 DastV8MaximumBulkBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 DastV8MaximumTableEntries = 1'048'575;
	inline constexpr uint64 DastV8MaximumStringBytes = 1024ull * 1024ull;
	inline constexpr uint64 DastV8MaximumContainerElements = 1'048'575;
	inline constexpr uint32 DastV8MaximumValueDepth = 64;

	enum class EDastV8Section : uint32
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

	struct FPackageWriterDiagnostic
	{
		EPackageWriterFailure Failure = EPackageWriterFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

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

	struct FPackageV8RegistryData
	{
		std::string PackageName;
		std::string AssetClass;
		bool bRedirect = false;
		std::string RedirectDestination;
		uint32 MainExportId = 0;
		uint32 ExportCount = 0;
		std::vector<std::string> HardPackageReferences;
		std::vector<std::string> SoftPackageReferences;
		std::vector<std::string> SearchableNames;
		uint64 ExternalBulkBytes = 0;
		FXxHash128 ExternalBulkHash;

		auto operator==(const FPackageV8RegistryData&) const -> bool = default;
	};

	struct FPackageReaderLimits
	{
		uint64 MaximumHeaderBytes = DastV8MaximumHeaderBytes;
		uint64 MaximumPackageBytes = DastV8MaximumPackageBytes;
		uint64 MaximumBulkBytes = DastV8MaximumBulkBytes;
		uint64 MaximumTableEntries = DastV8MaximumTableEntries;
		uint64 MaximumStringBytes = DastV8MaximumStringBytes;
		uint64 MaximumContainerElements = DastV8MaximumContainerElements;
		uint32 MaximumValueDepth = DastV8MaximumValueDepth;
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

	struct FPackageReaderDiagnostic
	{
		EPackageReaderFailure Failure = EPackageReaderFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

	// Freezes a detached linker model without emitting package bytes.
	COREDOBJECT_API auto FreezePackageV8(
		const FLinkerTables& Linker,
		FPackageWriterManifest& OutManifest,
		FPackageWriterDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Emits one atomic DURF/DAST v8 package image and optional raw bulk segment.
	COREDOBJECT_API auto WritePackageV8(
		const FLinkerTables& Linker,
		std::vector<std::byte>& OutPackageBytes,
		std::vector<std::byte>& OutBulkBytes,
		FPackageWriterDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Emits canonical main bytes from a linker whose external payloads may be descriptors only.
	COREDOBJECT_API auto WritePackageV8Main(
		const FLinkerTables& Linker,
		uint64 ExternalBulkBytes,
		FXxHash128 ExternalBulkHash,
		std::vector<std::byte>& OutPackageBytes,
		FPackageWriterDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Validates exactly the declared front matter and publishes package-level Registry data.
	COREDOBJECT_API auto ReadPackageV8Registry(
		std::span<const std::byte> FrontMatter,
		uint64 PhysicalPackageBytes,
		uint64 PhysicalBulkBytes,
		std::string_view PackageName,
		FPackageV8RegistryData& OutRegistry,
		FPackageReaderDiagnostic* OutDiagnostic = nullptr,
		const FPackageReaderLimits& Limits = {}) -> bool;

	// Validates complete canonical v8 main/bulk bytes and publishes detached linker tables.
	COREDOBJECT_API auto ReadPackageV8(
		std::span<const std::byte> PackageBytes,
		std::span<const std::byte> BulkBytes,
		std::string_view PackageName,
		FLinkerTables& OutLinker,
		FPackageReaderDiagnostic* OutDiagnostic = nullptr,
		const FPackageReaderLimits& Limits = {}) -> bool;

	// Validates canonical main bytes and publishes descriptor-only external BulkData values.
	// The caller must validate the external segment before publishing the linker.
	COREDOBJECT_API auto ReadPackageV8Metadata(
		std::span<const std::byte> PackageBytes,
		uint64 PhysicalBulkBytes,
		std::string_view PackageName,
		FLinkerTables& OutLinker,
		FPackageReaderDiagnostic* OutDiagnostic = nullptr,
		const FPackageReaderLimits& Limits = {}) -> bool;
}
