#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"

namespace Durin::ObjectPackage
{
	// Identifies a null, import-table, or export-table entry without exposing wire arithmetic.
	class FPackageIndex
	{
	public:
		enum class EKind : uint8
		{
			Null,
			Import,
			Export,
		};

		static auto Null() -> FPackageIndex { return {}; }
		COREDOBJECT_API static auto TryFromRaw(int64 Raw, FPackageIndex& Out) -> bool;
		COREDOBJECT_API static auto TryImport(uint64 ZeroBasedIndex, FPackageIndex& Out) -> bool;
		COREDOBJECT_API static auto TryExport(uint64 ZeroBasedIndex, FPackageIndex& Out) -> bool;

		auto GetKind() const -> EKind { return Kind; }
		auto IsNull() const -> bool { return Kind == EKind::Null; }
		auto IsImport() const -> bool { return Kind == EKind::Import; }
		auto IsExport() const -> bool { return Kind == EKind::Export; }
		auto GetTableIndex() const -> uint32 { return TableIndex; }
		COREDOBJECT_API auto ToRaw() const -> int64;

		auto operator<=>(const FPackageIndex&) const = default;

	private:
		EKind Kind = EKind::Null;
		uint32 TableIndex = 0;
	};

	// Format-neutral recursive identity of one serialized reflected value.
	enum class EValueKind : uint8
	{
		Bool,
		I8,
		I16,
		I32,
		I64,
		U8,
		U16,
		U32,
		U64,
		F32,
		F64,
		String,
		Name,
		Guid,
		Enum,
		Intrinsic,
		Struct,
		FixedArray,
		Array,
		Map,
		HardReference,
		SoftReference,
		Byte,
		Bytes,
		BulkData,
	};

	struct FSerializedType
	{
		EValueKind Kind = EValueKind::Bool;
		std::string QualifiedName;
		uint64 Parameter = 0;
		std::vector<FSerializedType> Children;

		COREDOBJECT_API auto operator==(const FSerializedType& Other) const -> bool;
		COREDOBJECT_API auto operator<=>(const FSerializedType& Other) const -> std::strong_ordering;
	};

	struct FSerializedField
	{
		std::string Name;
		FSerializedType Type;
		uint64 AuthoredFlags = 0;

		auto operator==(const FSerializedField&) const -> bool = default;
	};

	struct FSerializedSchema
	{
		std::string QualifiedName;
		std::vector<FSerializedField> Fields;

		auto operator==(const FSerializedSchema&) const -> bool = default;
	};

	enum class EPropertyProvenance : uint8
	{
		Implicit,
		Explicit,
		Forced,
	};

	// Detached logical value used by construct-free linkers and migration adapters.
	enum class EBulkStorageKind : uint8
	{
		Unset,
		Inline,
		External,
	};

	struct FSerializedValue
	{
		bool Bool = false;
		int64 Signed = 0;
		uint64 Unsigned = 0;
		uint64 FloatingBits = 0;
		std::string Text;
		uint32 NameNumber = 0;
		FGuid Guid;
		FByteBuffer Bytes;
		std::vector<uint64> ComponentBits;
		std::vector<FSerializedValue> Elements;
		std::vector<std::string> FieldNames;
		std::vector<EPropertyProvenance> Provenances;
		FPackageIndex Reference;
		uint32 BulkElementSize = 0;
		uint32 BulkAlignment = 0;
		uint64 BulkOffset = 0;
		uint64 BulkStoredSize = 0;
		FXxHash128 BulkContentHash;
		EBulkStorageKind BulkStorage = EBulkStorageKind::Unset;
		bool bBulkPayloadAvailable = true;

		auto operator==(const FSerializedValue&) const -> bool = default;
	};

	// Owns one tagged property and all detached data required to interpret it.
	struct FPropertyTag
	{
		std::string DeclaringType;
		std::string FieldName;
		FSerializedType Type;
		EPropertyProvenance Provenance = EPropertyProvenance::Implicit;
		FSerializedValue Value;
		FByteBuffer Payload;

		auto operator==(const FPropertyTag&) const -> bool = default;
	};

	struct FPackageImport
	{
		// Exact cross-package top-level or subobject target.
		FObjectPath ObjectPath;
		std::string ClassName;
		FPackageIndex Outer;

		auto operator==(const FPackageImport&) const -> bool = default;
	};

	struct FPackageExport
	{
		std::string ObjectName;
		std::string ClassName;
		FPackageIndex Outer;
		std::vector<FPropertyTag> Properties;

		auto operator==(const FPackageExport&) const -> bool = default;
	};

	struct FCustomVersion
	{
		FGuid Guid;
		uint32 Value = 0;
		std::optional<uint32> EmissionValue;
		std::optional<uint32> MaximumSupported;
		bool bCodecKnown = false;
		bool bRequiredForInterpretation = false;

		auto operator<=>(const FCustomVersion&) const = default;
	};

	struct FPackageSummary
	{
		FPackagePath PackagePath;
		struct FTopLevelAsset
		{
			FPackageIndex Export;
			FTopLevelAssetPath AssetPath;
			std::string ClassName;
			FObjectPath RedirectDestination;

			auto operator==(const FTopLevelAsset&) const -> bool = default;
		};
		std::vector<FTopLevelAsset> TopLevelAssets;
		std::vector<FPackagePath> HardPackageDependencies;
		std::vector<FPackagePath> SoftPackageDependencies;
		std::vector<std::string> SearchableNames;

		auto operator==(const FPackageSummary&) const -> bool = default;
	};

	enum class ELinkerFailure : uint8
	{
		None,
		InvalidIndex,
		InvalidType,
		InvalidTopology,
		LimitExceeded,
		UnsupportedValue,
	};

	struct FLinkerDiagnostic
	{
		ELinkerFailure Failure = ELinkerFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

	// Owns the format-neutral package tables consumed by save/load linkers.
	class FLinkerTables
	{
	public:
		FPackageSummary Summary;
		std::vector<std::string> Names;
		std::vector<FSerializedType> Types;
		std::vector<FSerializedSchema> Schemas;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FPackageImport> Imports;
		std::vector<FPackageExport> Exports;

		COREDOBJECT_API auto TryGetName(uint64 OneBasedIndex, std::string_view& Out) const -> bool;
		COREDOBJECT_API auto TryGetType(uint64 OneBasedIndex, const FSerializedType*& Out) const -> bool;
		COREDOBJECT_API auto TryGetSchema(uint64 OneBasedIndex, const FSerializedSchema*& Out) const -> bool;
		COREDOBJECT_API auto TryGetImport(FPackageIndex Index, const FPackageImport*& Out) const -> bool;
		COREDOBJECT_API auto TryGetExport(FPackageIndex Index, const FPackageExport*& Out) const -> bool;
		COREDOBJECT_API auto TryResolvePath(FPackageIndex Index, std::string& Out,
			FLinkerDiagnostic* OutDiagnostic = nullptr) const -> bool;

		auto operator==(const FLinkerTables&) const -> bool = default;
	};
}
