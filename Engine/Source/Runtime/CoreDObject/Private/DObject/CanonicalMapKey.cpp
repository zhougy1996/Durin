#include "DObject/CanonicalMapKey.h"

namespace Durin::ObjectPackage
{
	namespace
	{
		template<std::unsigned_integral T>
		auto AppendBigEndian(FByteBuffer& Out, T Value) -> void
		{
			for (size_t Index = sizeof(T); Index > 0; --Index)
				Out.push_back(static_cast<std::byte>(Value >> ((Index - 1) * 8)));
		}

		template<std::integral T>
		auto AppendSortableInteger(FByteBuffer& Out, T Value) -> void
		{
			using U = std::make_unsigned_t<T>;
			U Bits = std::bit_cast<U>(Value);
			if constexpr (std::is_signed_v<T>) Bits ^= U(1) << (sizeof(U) * 8 - 1);
			AppendBigEndian(Out, Bits);
		}

		template<std::unsigned_integral T>
		auto AppendSortableFloatBits(FByteBuffer& Out, T Bits) -> void
		{
			constexpr T Sign = T(1) << (sizeof(T) * 8 - 1);
			if ((Bits & ~Sign) == 0) Bits = 0;
			Bits = (Bits & Sign) ? ~Bits : (Bits ^ Sign);
			AppendBigEndian(Out, Bits);
		}

		auto Fail(ECanonicalMapKeyError Code, const FSerializedType& Type,
			const FSerializedValue& Value, size_t ActualCount = 0, size_t ExpectedCount = 0)
			-> FCanonicalMapKeyResult
		{
			return {{Code, Type.Kind, Type.Parameter, Value.Signed, Value.Unsigned, ActualCount, ExpectedCount}};
		}

		auto At(FCanonicalMapKeyResult Result, size_t Index) -> FCanonicalMapKeyResult
		{
			Result.Error.ValueRoute.insert(Result.Error.ValueRoute.begin(), Index);
			return Result;
		}

		auto IntegerWidth(EValueKind Kind) -> std::optional<ECanonicalIntegerWidth>
		{
			switch (Kind)
			{
			case EValueKind::I8: case EValueKind::U8: return ECanonicalIntegerWidth::One;
			case EValueKind::I16: case EValueKind::U16: return ECanonicalIntegerWidth::Two;
			case EValueKind::I32: case EValueKind::U32: return ECanonicalIntegerWidth::Four;
			case EValueKind::I64: case EValueKind::U64: return ECanonicalIntegerWidth::Eight;
			default: return std::nullopt;
			}
		}

		auto CanonicalKind(EValueKind Kind) -> std::optional<ECanonicalMapKeyKind>
		{
			switch (Kind)
			{
			case EValueKind::Bool: return ECanonicalMapKeyKind::Bool;
			case EValueKind::I8: return ECanonicalMapKeyKind::I8;
			case EValueKind::I16: return ECanonicalMapKeyKind::I16;
			case EValueKind::I32: return ECanonicalMapKeyKind::I32;
			case EValueKind::I64: return ECanonicalMapKeyKind::I64;
			case EValueKind::U8: return ECanonicalMapKeyKind::U8;
			case EValueKind::U16: return ECanonicalMapKeyKind::U16;
			case EValueKind::U32: return ECanonicalMapKeyKind::U32;
			case EValueKind::U64: return ECanonicalMapKeyKind::U64;
			case EValueKind::F32: return ECanonicalMapKeyKind::F32;
			case EValueKind::F64: return ECanonicalMapKeyKind::F64;
			case EValueKind::String: return ECanonicalMapKeyKind::String;
			case EValueKind::Name: return ECanonicalMapKeyKind::Name;
			case EValueKind::Guid: return ECanonicalMapKeyKind::Guid;
			case EValueKind::Enum: return ECanonicalMapKeyKind::Enum;
			case EValueKind::Intrinsic: case EValueKind::Struct: return ECanonicalMapKeyKind::Struct;
			case EValueKind::Byte: return ECanonicalMapKeyKind::Byte;
			default: return std::nullopt;
			}
		}

		auto SignedFits(EValueKind Kind, int64 Value) -> bool
		{
			switch (Kind)
			{
			case EValueKind::I8: return Value >= std::numeric_limits<int8>::min() && Value <= std::numeric_limits<int8>::max();
			case EValueKind::I16: return Value >= std::numeric_limits<int16>::min() && Value <= std::numeric_limits<int16>::max();
			case EValueKind::I32: return Value >= std::numeric_limits<int32>::min() && Value <= std::numeric_limits<int32>::max();
			case EValueKind::I64: return true;
			default: return false;
			}
		}

		auto UnsignedFits(EValueKind Kind, uint64 Value) -> bool
		{
			switch (Kind)
			{
			case EValueKind::U8: return Value <= std::numeric_limits<uint8>::max();
			case EValueKind::U16: return Value <= std::numeric_limits<uint16>::max();
			case EValueKind::U32: return Value <= std::numeric_limits<uint32>::max();
			case EValueKind::U64: return true;
			default: return false;
			}
		}

		auto AppendValue(const FSerializedType& Type, const FSerializedValue& Value,
			FCanonicalMapKeyWriter& Writer) -> FCanonicalMapKeyResult;

		auto AppendIntrinsic(const FSerializedType& Type, const FSerializedValue& Value,
			FCanonicalMapKeyWriter& Writer) -> FCanonicalMapKeyResult
		{
			Writer.WriteType(ECanonicalMapKeyKind::Struct);
			if (Type.Parameter == 5)
			{
				if (Value.ComponentBits.size() != 10)
					return Fail(ECanonicalMapKeyError::TransformComponentCount, Type, Value, Value.ComponentBits.size(), 10);
				for (const auto [Ordinal, Layout, Offset, Count] : {
					std::tuple<uint32, uint64, size_t, size_t>{0, 4, 0, 4},
					std::tuple<uint32, uint64, size_t, size_t>{1, 2, 4, 3},
					std::tuple<uint32, uint64, size_t, size_t>{2, 2, 7, 3}})
				{
					Writer.WriteStructField(Ordinal, 0);
					FSerializedType Child{.Kind = EValueKind::Intrinsic, .Parameter = Layout};
					FSerializedValue ChildValue;
					ChildValue.ComponentBits.assign(
						Value.ComponentBits.begin() + static_cast<ptrdiff_t>(Offset),
						Value.ComponentBits.begin() + static_cast<ptrdiff_t>(Offset + Count));
					if (auto Result = AppendIntrinsic(Child, ChildValue, Writer); !Result) return At(std::move(Result), Ordinal);
				}
				return {};
			}

			const uint64 Count = Type.Parameter == 1 ? 2 :
				(Type.Parameter == 2 ? 3 : (Type.Parameter == 3 || Type.Parameter == 4 || Type.Parameter == 6 ? 4 : 0));
			if (Count == 0 || Value.ComponentBits.size() != Count)
				return Fail(ECanonicalMapKeyError::IntrinsicLayout, Type, Value, Value.ComponentBits.size(), Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Writer.WriteStructField(Index, 0);
				if (Type.Parameter == 6)
				{
					Writer.WriteType(ECanonicalMapKeyKind::F32);
					Writer.WriteFloat32Bits(static_cast<uint32>(Value.ComponentBits[Index]));
				}
				else
				{
					Writer.WriteType(ECanonicalMapKeyKind::F64);
					Writer.WriteFloat64Bits(Value.ComponentBits[Index]);
				}
			}
			return {};
		}

		auto AppendStruct(const FSerializedType& Type, const FSerializedValue& Value,
			FCanonicalMapKeyWriter& Writer) -> FCanonicalMapKeyResult
		{
			if (Type.Children.size() != Value.Elements.size())
				return Fail(ECanonicalMapKeyError::StructFieldCount, Type, Value, Value.Elements.size(), Type.Children.size());
			Writer.WriteType(ECanonicalMapKeyKind::Struct);
			for (size_t Ordinal = 0; Ordinal < Type.Children.size(); ++Ordinal)
			{
				const FSerializedType& Child = Type.Children[Ordinal];
				const FSerializedValue& ChildValue = Value.Elements[Ordinal];
				if (Child.Kind == EValueKind::FixedArray)
				{
					if (Child.Children.size() != 1 || Child.Parameter != ChildValue.Elements.size())
						return At(Fail(ECanonicalMapKeyError::FixedArrayShape, Child, ChildValue, ChildValue.Elements.size(), Child.Parameter), Ordinal);
					for (size_t Index = 0; Index < ChildValue.Elements.size(); ++Index)
					{
						Writer.WriteStructField(static_cast<uint32>(Ordinal), static_cast<uint32>(Index));
						if (auto Result = AppendValue(Child.Children.front(), ChildValue.Elements[Index], Writer); !Result)
							return At(At(std::move(Result), Index), Ordinal);
					}
				}
				else
				{
					Writer.WriteStructField(static_cast<uint32>(Ordinal), 0);
					if (auto Result = AppendValue(Child, ChildValue, Writer); !Result) return At(std::move(Result), Ordinal);
				}
			}
			return {};
		}

		auto AppendValue(const FSerializedType& Type, const FSerializedValue& Value,
			FCanonicalMapKeyWriter& Writer) -> FCanonicalMapKeyResult
		{
			if (Type.Kind == EValueKind::Intrinsic) return AppendIntrinsic(Type, Value, Writer);
			if (Type.Kind == EValueKind::Struct) return AppendStruct(Type, Value, Writer);
			const auto Tag = CanonicalKind(Type.Kind);
			if (!Tag) return Fail(ECanonicalMapKeyError::UnsupportedType, Type, Value);
			Writer.WriteType(*Tag);
			switch (Type.Kind)
			{
			case EValueKind::Bool: Writer.WriteBool(Value.Bool); return {};
			case EValueKind::I8: case EValueKind::I16: case EValueKind::I32: case EValueKind::I64:
				if (!SignedFits(Type.Kind, Value.Signed))
					return Fail(ECanonicalMapKeyError::SignedOutOfRange, Type, Value);
				Writer.WriteSigned(Value.Signed, *IntegerWidth(Type.Kind)); return {};
			case EValueKind::U8: case EValueKind::U16: case EValueKind::U32: case EValueKind::U64:
				if (!UnsignedFits(Type.Kind, Value.Unsigned))
					return Fail(ECanonicalMapKeyError::UnsignedOutOfRange, Type, Value);
				Writer.WriteUnsigned(Value.Unsigned, *IntegerWidth(Type.Kind)); return {};
			case EValueKind::F32: Writer.WriteFloat32Bits(static_cast<uint32>(Value.FloatingBits)); return {};
			case EValueKind::F64: Writer.WriteFloat64Bits(Value.FloatingBits); return {};
			case EValueKind::String: Writer.WriteString(Value.Text); return {};
			case EValueKind::Name: Writer.WriteName(Value.Text, Value.NameNumber); return {};
			case EValueKind::Guid: Writer.WriteGuid(Value.Guid); return {};
			case EValueKind::Byte:
				if (Value.Unsigned > std::numeric_limits<uint8>::max())
					return Fail(ECanonicalMapKeyError::ByteOutOfRange, Type, Value);
				Writer.WriteUnsigned(Value.Unsigned, ECanonicalIntegerWidth::One); return {};
			case EValueKind::Enum:
			{
				const EValueKind Storage = static_cast<EValueKind>(Type.Parameter);
				const auto Width = IntegerWidth(Storage);
				if (!Width) return Fail(ECanonicalMapKeyError::InvalidEnumStorage, Type, Value);
				if (Storage >= EValueKind::I8 && Storage <= EValueKind::I64)
				{
					if (!SignedFits(Storage, Value.Signed))
						return Fail(ECanonicalMapKeyError::EnumOutOfRange, Type, Value);
					Writer.WriteSigned(Value.Signed, *Width);
				}
				else
				{
					if (!UnsignedFits(Storage, Value.Unsigned))
						return Fail(ECanonicalMapKeyError::EnumOutOfRange, Type, Value);
					Writer.WriteUnsigned(Value.Unsigned, *Width);
				}
				return {};
			}
			default: return Fail(ECanonicalMapKeyError::UnsupportedType, Type, Value);
			}
		}
	}

	auto FCanonicalMapKeyWriter::WriteType(ECanonicalMapKeyKind Kind) -> void
	{
		Bytes.push_back(static_cast<std::byte>(Kind));
	}

	auto FCanonicalMapKeyWriter::WriteBool(bool Value) -> void
	{
		Bytes.push_back(Value ? std::byte{1} : std::byte{0});
	}

	auto FCanonicalMapKeyWriter::WriteSigned(int64 Value, ECanonicalIntegerWidth Width) -> void
	{
		switch (Width)
		{
		case ECanonicalIntegerWidth::One: AppendSortableInteger(Bytes, static_cast<int8>(Value)); break;
		case ECanonicalIntegerWidth::Two: AppendSortableInteger(Bytes, static_cast<int16>(Value)); break;
		case ECanonicalIntegerWidth::Four: AppendSortableInteger(Bytes, static_cast<int32>(Value)); break;
		case ECanonicalIntegerWidth::Eight: AppendSortableInteger(Bytes, Value); break;
		}
	}

	auto FCanonicalMapKeyWriter::WriteUnsigned(uint64 Value, ECanonicalIntegerWidth Width) -> void
	{
		switch (Width)
		{
		case ECanonicalIntegerWidth::One: AppendSortableInteger(Bytes, static_cast<uint8>(Value)); break;
		case ECanonicalIntegerWidth::Two: AppendSortableInteger(Bytes, static_cast<uint16>(Value)); break;
		case ECanonicalIntegerWidth::Four: AppendSortableInteger(Bytes, static_cast<uint32>(Value)); break;
		case ECanonicalIntegerWidth::Eight: AppendSortableInteger(Bytes, Value); break;
		}
	}

	auto FCanonicalMapKeyWriter::WriteFloat32Bits(uint32 Bits) -> void
	{
		AppendSortableFloatBits(Bytes, Bits);
	}

	auto FCanonicalMapKeyWriter::WriteFloat64Bits(uint64 Bits) -> void
	{
		AppendSortableFloatBits(Bytes, Bits);
	}

	auto FCanonicalMapKeyWriter::WriteString(std::string_view Value) -> void
	{
		AppendBigEndian(Bytes, static_cast<uint64>(Value.size()));
		const auto ValueBytes = std::as_bytes(std::span(Value));
		Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
	}

	auto FCanonicalMapKeyWriter::WriteName(std::string_view PlainName, uint32 Number) -> void
	{
		WriteString(PlainName);
		AppendBigEndian(Bytes, Number);
	}

	auto FCanonicalMapKeyWriter::WriteGuid(const FGuid& Value) -> void
	{
		AppendBigEndian(Bytes, Value.A);
		AppendBigEndian(Bytes, Value.B);
		AppendBigEndian(Bytes, Value.C);
		AppendBigEndian(Bytes, Value.D);
	}

	auto FCanonicalMapKeyWriter::WriteStructField(uint32 Ordinal, uint32 ArrayIndex) -> void
	{
		AppendBigEndian(Bytes, Ordinal);
		AppendBigEndian(Bytes, ArrayIndex);
	}

	auto BuildCanonicalMapKeyToken(const FSerializedType& Type, const FSerializedValue& Value,
		FByteBuffer& OutToken) -> FCanonicalMapKeyResult
	{
		FCanonicalMapKeyWriter Writer;
		if (auto Result = AppendValue(Type, Value, Writer); !Result) return Result;
		OutToken = Writer.TakeBytes();
		return {};
	}

	auto FormatCanonicalMapKeyError(const FCanonicalMapKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ECanonicalMapKeyError::None: return {};
		case ECanonicalMapKeyError::TransformComponentCount: return "CanonicalMapKeyInvalidValue: transform component count is invalid.";
		case ECanonicalMapKeyError::IntrinsicLayout: return "CanonicalMapKeyInvalidValue: intrinsic layout or component count is invalid.";
		case ECanonicalMapKeyError::StructFieldCount: return "CanonicalMapKeyInvalidValue: struct fields do not match their type.";
		case ECanonicalMapKeyError::FixedArrayShape: return "CanonicalMapKeyInvalidValue: fixed-array field shape is invalid.";
		case ECanonicalMapKeyError::UnsupportedType: return "CanonicalMapKeyUnsupported: value type is not canonicalizable.";
		case ECanonicalMapKeyError::SignedOutOfRange: return "CanonicalMapKeyInvalidValue: signed value is out of range.";
		case ECanonicalMapKeyError::UnsignedOutOfRange: return "CanonicalMapKeyInvalidValue: unsigned value is out of range.";
		case ECanonicalMapKeyError::ByteOutOfRange: return "CanonicalMapKeyInvalidValue: byte value is out of range.";
		case ECanonicalMapKeyError::InvalidEnumStorage: return "CanonicalMapKeyUnsupported: enum storage type is invalid.";
		case ECanonicalMapKeyError::EnumOutOfRange: return "CanonicalMapKeyInvalidValue: enum value is out of range.";
		}
		return {};
	}
}
