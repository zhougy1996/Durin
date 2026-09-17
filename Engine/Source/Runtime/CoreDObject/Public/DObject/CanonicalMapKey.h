#pragma once

#include "CoreDObjectAPI.h"
#include "DObject/PackageLinker.h"

namespace Durin::ObjectPackage
{
	// Stable token tags retained from the reflected property-kind contract.
	enum class ECanonicalMapKeyKind : uint8
	{
		Bool = 1,
		I8 = 2,
		I16 = 3,
		I32 = 4,
		I64 = 5,
		U8 = 6,
		U16 = 7,
		U32 = 8,
		U64 = 9,
		F32 = 10,
		F64 = 11,
		String = 12,
		Enum = 13,
		Struct = 17,
		Name = 18,
		Guid = 19,
		Byte = 22,
	};

	enum class ECanonicalIntegerWidth : uint8
	{
		One = 1,
		Two = 2,
		Four = 4,
		Eight = 8,
	};

	// Appends canonical key primitives using ordering-preserving byte encodings.
	class FCanonicalMapKeyWriter
	{
	public:
		COREDOBJECT_API auto WriteType(ECanonicalMapKeyKind Kind) -> void;
		COREDOBJECT_API auto WriteBool(bool Value) -> void;
		COREDOBJECT_API auto WriteSigned(int64 Value, ECanonicalIntegerWidth Width) -> void;
		COREDOBJECT_API auto WriteUnsigned(uint64 Value, ECanonicalIntegerWidth Width) -> void;
		COREDOBJECT_API auto WriteFloat32Bits(uint32 Bits) -> void;
		COREDOBJECT_API auto WriteFloat64Bits(uint64 Bits) -> void;
		COREDOBJECT_API auto WriteString(std::string_view Value) -> void;
		COREDOBJECT_API auto WriteName(std::string_view PlainName, uint32 Number) -> void;
		COREDOBJECT_API auto WriteGuid(const FGuid& Value) -> void;
		COREDOBJECT_API auto WriteStructField(uint32 Ordinal, uint32 ArrayIndex) -> void;

		auto GetBytes() const -> FByteView { return Bytes; }
		auto TakeBytes() -> FByteBuffer { return std::move(Bytes); }

	private:
		FByteBuffer Bytes;
	};

	enum class ECanonicalMapKeyError : uint8
	{
		None, TransformComponentCount, IntrinsicLayout, StructFieldCount,
		FixedArrayShape, UnsupportedType, SignedOutOfRange, UnsignedOutOfRange,
		ByteOutOfRange, InvalidEnumStorage, EnumOutOfRange,
	};

	struct FCanonicalMapKeyError
	{
		ECanonicalMapKeyError Code = ECanonicalMapKeyError::None;
		EValueKind Kind = EValueKind::Bool;
		uint64 TypeParameter = 0;
		int64 SignedValue = 0;
		uint64 UnsignedValue = 0;
		size_t ActualCount = 0;
		size_t ExpectedCount = 0;
		// Outer-to-inner field ordinals and fixed-array indices.
		std::vector<size_t> ValueRoute;
		auto HasError() const -> bool { return Code != ECanonicalMapKeyError::None; }
	};

	struct FCanonicalMapKeyResult
	{
		FCanonicalMapKeyError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};

	COREDOBJECT_API auto FormatCanonicalMapKeyError(const FCanonicalMapKeyError& Error) -> std::string;

	// Builds one detached canonical token and replaces output only after complete success.
	COREDOBJECT_API auto BuildCanonicalMapKeyToken(
		const FSerializedType& Type,
		const FSerializedValue& Value,
		FByteBuffer& OutToken) -> FCanonicalMapKeyResult;
}
