#pragma once

#include "PackageV4WireContract.h"

#include <compare>
#include <memory>
#include <optional>
#include <set>

namespace Durin::Testing::DastV4
{
	constexpr uint64 MaximumTableEntries = 1048575;
	constexpr uint64 MaximumSchemaFields = 65535;
	constexpr uint64 MaximumCustomVersions = 256;
	constexpr uint64 MaximumContainerElements = 1048575;
	constexpr uint32 MaximumValueDepth = 64;

	enum class ETypeOpcode : uint8
	{
		Bool = 0x01,
		I8 = 0x02,
		I16 = 0x03,
		I32 = 0x04,
		I64 = 0x05,
		U8 = 0x06,
		U16 = 0x07,
		U32 = 0x08,
		U64 = 0x09,
		F32 = 0x0a,
		F64 = 0x0b,
		String = 0x0c,
		Name = 0x0d,
		Guid = 0x0e,
		Enum = 0x0f,
		Intrinsic = 0x10,
		Struct = 0x11,
		FixedArray = 0x12,
		Array = 0x13,
		Map = 0x14,
		HardRef = 0x15,
		SoftRef = 0x16,
		Bytes = 0x17,
	};

	struct FTypeDescriptor;
	using FTypePtr = std::shared_ptr<FTypeDescriptor>;

	struct FTypeDescriptor
	{
		ETypeOpcode Opcode = ETypeOpcode::Bool;
		std::string QualifiedName;
		uint64 Parameter = 0;
		std::vector<FTypePtr> Children;
		bool HasDeterministicStructOperations = true;
		bool HasCustomSerializer = false;
	};

	auto MakeType(
		ETypeOpcode Opcode,
		std::string QualifiedName = {},
		uint64 Parameter = 0,
		std::vector<FTypePtr> Children = {}) -> FTypePtr;

	struct FGuidValue
	{
		uint32 A = 0;
		uint32 B = 0;
		uint32 C = 0;
		uint32 D = 0;

		auto operator<=>(const FGuidValue&) const = default;
	};

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
		FGuidValue Guid;
		uint32 Value = 0;
		std::optional<uint32> EmissionValue;
		std::optional<uint32> MaximumSupported;
		bool CodecKnown = false;
		bool RequiredForInterpretation = false;
	};

	struct FObjectDescriptor
	{
		std::string Path;
		std::string OuterPath;
		std::string ClassName;
		std::string ObjectName;
	};

	struct FTableInput
	{
		uint64 PublicDependencyCount = 0;
		std::vector<std::string> AdditionalNames;
		std::vector<FTypePtr> Types;
		std::vector<FSchemaDescriptor> Schemas;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FObjectDescriptor> Objects;
	};

	struct FFrozenType
	{
		FTypePtr Descriptor;
		std::vector<uint8> StructuralKey;
	};

	struct FFrozenTables
	{
		uint64 PublicDependencyCount = 0;
		std::vector<std::string> Names;
		std::vector<FFrozenType> Types;
		std::vector<FSchemaDescriptor> Schemas;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FObjectDescriptor> Objects;

		auto NameId(std::string_view Name) const -> uint64;
		auto TypeId(const FTypeDescriptor& Type) const -> uint64;
		auto SchemaId(std::string_view Name) const -> uint64;
		auto FieldId(uint64 SchemaId, std::string_view Name) const -> uint64;
		auto ObjectId(std::string_view Path) const -> uint64;
	};

	class FDiscoveryRegistry
	{
	public:
		auto AddName(std::string Name, std::string& OutError) -> bool;
		auto Freeze() -> void { Frozen = true; }
		auto Names() const -> const std::set<std::string>& { return DiscoveredNames; }

	private:
		bool Frozen = false;
		std::set<std::string> DiscoveredNames;
	};

	auto FreezeTables(
		const FTableInput& Input,
		FFrozenTables& OutTables,
		std::string& OutError) -> bool;
	auto EncodeTableSections(
		const FFrozenTables& Tables,
		std::array<std::vector<uint8>, 4>& OutSections,
		std::string& OutError) -> bool;
	auto DecodeTableSections(
		const std::array<std::vector<uint8>, 4>& Sections,
		FFrozenTables& OutTables,
		std::string& OutError) -> bool;

	struct FValue
	{
		bool Bool = false;
		uint64 Unsigned = 0;
		int64 Signed = 0;
		double Number = 0;
		std::string Text;
		FGuidValue Guid;
		std::vector<uint8> ByteData;
		std::vector<double> Components;
		std::vector<FValue> Elements;
		std::vector<uint64> FieldIds;
		std::vector<uint8> Provenances;
		uint8 ReferenceTag = 0;
		uint64 ReferenceId = 0;

		auto operator==(const FValue&) const -> bool = default;
	};

	auto EncodeValue(
		const FTypeDescriptor& Type,
		const FValue& Value,
		const FFrozenTables& Tables,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	auto DecodeValue(
		const FTypeDescriptor& Type,
		std::span<const uint8> Bytes,
		const FFrozenTables& Tables,
		FValue& OutValue,
		std::string& OutError) -> bool;

	struct FOverrideCandidate
	{
		std::string SchemaName;
		std::string FieldName;
		FValue Value;
		FValue DefaultValue;
		bool LoadedExplicit = false;
		bool Forced = false;
	};

	auto EncodeOverrideBlock(
		std::span<const FOverrideCandidate> Candidates,
		const FFrozenTables& Tables,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;

	struct FObjectValueInput
	{
		std::string ObjectPath;
		std::vector<FOverrideCandidate> Overrides;
	};

	auto EncodeValueSection(
		std::span<const FObjectValueInput> Objects,
		const FFrozenTables& Tables,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	auto ValidateValueSection(
		std::span<const uint8> Bytes,
		const FFrozenTables& Tables,
		std::string& OutError) -> bool;

	auto EncodeRetainedClosure(
		const FFrozenTables& Tables,
		uint64 RootSchemaId,
		uint64 RootFieldId,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	auto ValidateRetainedClosure(
		std::span<const uint8> Bytes,
		std::string& OutError) -> bool;
	auto EncodeUnknownValueBody(
		std::span<const uint8> Closure,
		std::span<const uint8> Payload,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	auto ValidateUnknownValueBody(
		std::span<const uint8> Bytes,
		std::vector<uint8>& OutClosure,
		std::vector<uint8>& OutPayload,
		std::string& OutError) -> bool;
}
