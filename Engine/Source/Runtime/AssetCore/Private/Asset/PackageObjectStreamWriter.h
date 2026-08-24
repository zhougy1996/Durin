#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Catalog.h"
#include "Asset/PackageAuthoring.h"
#include "Asset/PackageVersionPolicy.h"
#include "DObject/DefaultDeltaPlan.h"

#include <memory>
#include <optional>

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

	ASSETCORE_API auto MakeType(
		ETypeOpcode Opcode,
		std::string QualifiedName = {},
		uint64 Parameter = 0,
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

	// Validates and writes to temporary owned storage. OutBytes is replaced only
	// after the complete package, including retained closures, is valid.
	ASSETCORE_API auto WritePackage(
		const FPackageInput& Input,
		std::vector<std::byte>& OutBytes,
		FWriterDiagnostic* OutDiagnostic = nullptr) -> bool;

	struct FAssetPackageWriteOptions
	{
		EDefaultDeltaMode DeltaMode = EDefaultDeltaMode::Enabled;
		FAssetPackageSerializationOptions Serialization;
		bool bVerifyRepeatedEncoding = false;
	};

	// Production integration boundary shared by ordinary serialization, saves,
	// and explicit callers that need writer diagnostics or delta-mode control.
	ASSETCORE_API auto WriteAssetPackage(
		DPackage* Package,
		std::vector<std::byte>& OutBytes,
		const FAssetPackageWriteOptions& Options = {},
		FWriterDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	ASSETCORE_API auto WriteRedirectorPackage(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		std::vector<std::byte>& OutBytes) -> FAssetResult;
}
