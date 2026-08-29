#pragma once

#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/References.h"
#include "DObject/DefaultDeltaPlan.h"

namespace Durin::Asset::PackageObjectStream
{
	inline constexpr uint32 Magic = DastPackageMagic;
	inline constexpr uint32 Version = AssetPackageObjectStreamVersion;
	inline constexpr uint64 MaximumPackageBytes = 256ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumStringBytes = 1024ull * 1024ull;
	inline constexpr uint64 MaximumTableEntries = 1'048'575;
	inline constexpr uint64 MaximumSchemaFields = 65'535;
	inline constexpr uint64 MaximumCustomVersions = 256;
	inline constexpr uint64 MaximumDependencies = 4'096;
	inline constexpr uint64 MaximumContainerElements = 1'048'575;
	inline constexpr uint64 MaximumByteValueBytes = MaximumPackageBytes;
	inline constexpr uint32 MaximumValueDepth = 64;

	enum class EWriterFailure : uint8
	{
		None,
		InvalidInput,
		InvalidUtf8,
		LimitExceeded,
		DuplicateInput,
		DescriptorCycle,
		InvalidTopology,
		MissingDiscovery,
		ManifestMismatch,
		UnsupportedType,
		InvalidValue,
		InvalidRetainedClosure,
		PackageTooLarge,
		DeltaPlanFailure,
		ArchiveFailure,
	};

	struct FWriterDiagnostic
	{
		EWriterFailure Failure = EWriterFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

	enum class ETypeOpcode : uint8
	{
		Bool = 0x01, I8 = 0x02, I16 = 0x03, I32 = 0x04, I64 = 0x05,
		U8 = 0x06, U16 = 0x07, U32 = 0x08, U64 = 0x09,
		F32 = 0x0a, F64 = 0x0b, String = 0x0c, Name = 0x0d,
		Guid = 0x0e, Enum = 0x0f, Intrinsic = 0x10, Struct = 0x11,
		FixedArray = 0x12, Array = 0x13, Map = 0x14,
		HardRef = 0x15, SoftRef = 0x16, Bytes = 0x17, BulkData = 0x18,
	};

	struct FTypeDescriptor;
	using FTypePtr = std::shared_ptr<FTypeDescriptor>;

	struct FTypeDescriptor
	{
		ETypeOpcode Opcode = ETypeOpcode::Bool;
		std::string QualifiedName;
		uint64 Parameter = 0;
		std::vector<FTypePtr> Children;
		bool bHasDeterministicStructOperations = true;
		bool bHasCustomSerializer = false;
	};

	ASSETREGISTRY_API auto MakeType(ETypeOpcode Opcode,
		std::string QualifiedName = {}, uint64 Parameter = 0,
		std::vector<FTypePtr> Children = {}) -> FTypePtr;

	struct FFieldDescriptor
	{
		std::string Name;
		FTypePtr Type;
		uint64 AuthoredFlags = 0;
	};

	struct FSchemaDescriptor
	{
		std::string QualifiedName;
		std::vector<FFieldDescriptor> Fields;
	};

	struct FCustomVersion
	{
		FGuid Guid;
		uint32 Value = 0;
		std::optional<uint32> EmissionValue;
		std::optional<uint32> MaximumSupported;
		bool bCodecKnown = false;
		bool bRequiredForInterpretation = false;
	};

	struct FObjectDescriptor
	{
		std::string Path;
		std::string OuterPath;
		std::string ClassName;
		std::string ObjectName;
	};

	struct FValue
	{
		bool Bool = false;
		uint64 Unsigned = 0;
		int64 Signed = 0;
		uint64 FloatingBits = 0;
		std::string Text;
		FGuid Guid;
		std::vector<std::byte> Bytes;
		std::vector<uint64> ComponentBits;
		std::vector<FValue> Elements;
		std::vector<std::string> FieldNames;
		std::vector<EDefaultDeltaProvenance> Provenances;
		uint8 ReferenceTag = 0;
		uint64 ReferenceId = 0;
	};

	struct FKnownOverride
	{
		std::string SchemaName;
		std::string FieldName;
		EDefaultDeltaProvenance Provenance = EDefaultDeltaProvenance::Explicit;
		FValue Value;
	};

	struct FRetainedUnknownOverride
	{
		std::string SchemaName;
		std::string FieldName;
		std::vector<std::byte> DescriptorClosure;
		std::vector<std::byte> Payload;
	};

	struct FObjectValueInput
	{
		std::string ObjectPath;
		std::vector<FKnownOverride> KnownOverrides;
		std::vector<FRetainedUnknownOverride> RetainedUnknownOverrides;
	};

	struct FPackageInput
	{
		std::string AssetClass;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::string RedirectDestination;
		std::vector<std::string> Dependencies;
		std::vector<std::string> AdditionalNames;
		std::vector<FTypePtr> Types;
		std::vector<FSchemaDescriptor> Schemas;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FObjectDescriptor> Objects;
		std::vector<FObjectValueInput> ObjectValues;
	};

	// Canonical construct-free DAST object-stream writer. Used by the reader to
	// prove canonical byte identity without constructing Engine asset objects.
	ASSETREGISTRY_API auto WritePackage(const FPackageInput& Input,
		std::vector<std::byte>& OutBytes,
		FWriterDiagnostic* OutDiagnostic = nullptr) -> bool;

	inline constexpr uint32 MaximumSummaryBytes = 65'535;
	inline constexpr uint8 RequiredSectionCount = 5;
	inline constexpr uint64 MaximumHeaderBytes =
		13ull + MaximumSummaryBytes + RequiredSectionCount * 9ull;

	enum class EReaderFailure : uint8
	{
		None,
		TruncatedInput,
		InvalidPrimitive,
		InvalidUtf8,
		LimitExceeded,
		InvalidHeader,
		InvalidDirectory,
		InvalidTable,
		DescriptorCycle,
		InvalidTopology,
		InvalidValue,
		InvalidRetainedClosure,
		NonCanonical,
		UnknownClass,
		MissingDependency,
		TypeMismatch,
		ArchiveFailure,
		PostLoadFailure,
		PublicationFailure,
	};

	struct FReaderDiagnostic
	{
		EReaderFailure Failure = EReaderFailure::None;
		std::string LogicalPath;
		std::string Message;
		uint64 ByteOffset = 0;

		auto Reset() -> void { *this = {}; }
	};

	struct FReaderLimits
	{
		uint64 PackageBytes = MaximumPackageBytes;
		uint64 StringBytes = MaximumStringBytes;
		uint64 TableEntries = MaximumTableEntries;
		uint64 SchemaFields = MaximumSchemaFields;
		uint64 CustomVersions = MaximumCustomVersions;
		uint64 Dependencies = MaximumDependencies;
		uint64 ContainerElements = MaximumContainerElements;
		uint64 ByteValueBytes = MaximumByteValueBytes;
		uint32 ValueDepth = MaximumValueDepth;
	};

	enum class ESectionKind : uint8
	{
		Name = 0x01,
		Type = 0x02,
		Schema = 0x03,
		Object = 0x04,
		Value = 0x05,
	};

	struct FSectionDirectoryEntry
	{
		ESectionKind Kind = ESectionKind::Name;
		uint32 Offset = 0;
		uint32 Length = 0;

		auto operator==(const FSectionDirectoryEntry&) const -> bool = default;
	};

	struct FValidatedHeader
	{
		std::string AssetClass;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::string RedirectDestination;
		std::vector<std::string> Dependencies;
		uint64 ObjectCount = 0;
		std::array<FSectionDirectoryEntry, RequiredSectionCount> Sections;
		uint64 BytesRead = 0;

		auto operator==(const FValidatedHeader&) const -> bool = default;
	};

	struct FDecodedType
	{
		ETypeOpcode Opcode = ETypeOpcode::Bool;
		std::string QualifiedName;
		uint64 Parameter = 0;
		std::vector<uint64> ChildTypeIds;
	};

	struct FDecodedField
	{
		std::string Name;
		uint64 TypeId = 0;
		uint64 AuthoredFlags = 0;
	};

	struct FDecodedSchema
	{
		std::string QualifiedName;
		std::vector<FDecodedField> Fields;
	};

	struct FDecodedObject
	{
		uint64 Id = 0;
		uint64 OuterId = 0;
		std::string Path;
		std::string ClassName;
		std::string ObjectName;
	};

	struct FDecodedOverride
	{
		uint64 SchemaId = 0;
		uint64 FieldId = 0;
		uint8 Provenance = 0;
		FValue Value;
		std::vector<std::byte> DescriptorClosure;
		std::vector<std::byte> RetainedPayload;
		uint64 PayloadOffset = 0;
		uint64 PayloadSize = 0;
	};

	struct FDecodedObjectValues
	{
		std::vector<FDecodedOverride> Overrides;
	};

	struct FDecodedPackage
	{
		FValidatedHeader Header;
		std::vector<std::string> Names;
		std::vector<FDecodedType> Types;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FDecodedSchema> Schemas;
		std::vector<FDecodedObject> Objects;
		std::vector<FDecodedObjectValues> ObjectValues;
	};

	ASSETREGISTRY_API auto ReadHeader(std::span<const std::byte> Bytes,
		FValidatedHeader& OutHeader, const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr, uint64 PackageSize = 0) -> bool;
	ASSETREGISTRY_API auto DecodePackage(std::span<const std::byte> Bytes,
		FDecodedPackage& OutPackage, const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> bool;
	// Structural decoding is shared with Engine live load, which canonicalizes
	// recognized reflection aliases before applying values.
	ASSETREGISTRY_API auto DecodePackageStructure(std::span<const std::byte> Bytes,
		FDecodedPackage& OutPackage, const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> bool;
	ASSETREGISTRY_API auto ReencodePackage(const FDecodedPackage& Package,
		std::vector<std::byte>& OutBytes,
		FReaderDiagnostic* OutDiagnostic = nullptr) -> bool;
	ASSETREGISTRY_API auto ExtractReferences(std::span<const std::byte> Bytes,
		const FAssetPath& SourcePackage,
		std::vector<FAssetReferenceEdge>& OutReferences,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;
	// Validates a complete DAST v6 envelope and reconstructs its canonical
	// construct-free logical object stream.
	ASSETREGISTRY_API auto ExtractDastObjectStream(
		std::span<const std::byte> PackageBytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult;
	ASSETREGISTRY_API auto ExtractAssetPackageReferences(
		std::span<const std::byte> PackageBytes,
		const FAssetPath& SourcePackage,
		std::vector<FAssetReferenceEdge>& OutReferences,
		FAssetPackageFingerprint* OutFingerprint = nullptr) -> FAssetResult;
	ASSETREGISTRY_API auto ResetReencodeCountForTesting() -> void;
	ASSETREGISTRY_API auto GetReencodeCountForTesting() -> uint64;
}
