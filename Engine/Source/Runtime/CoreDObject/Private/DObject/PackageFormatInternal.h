#pragma once

#include "CoreMinimal.h"

#include "DObject/PackageFormat.h"

namespace Durin::ObjectPackage::Private
{
	inline auto BytewiseLess(std::string_view Left, std::string_view Right) -> bool
	{
		return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end(),
			[](char A, char B) { return static_cast<uint8>(A) < static_cast<uint8>(B); });
	}

	inline auto SignedFits(EValueKind Kind, int64 Value) -> bool
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

	inline auto UnsignedFits(EValueKind Kind, uint64 Value) -> bool
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

	inline auto IsValidUtf8(std::string_view Value) -> bool
	{
		for (size_t Index = 0; Index < Value.size();)
		{
			const uint8 Lead = static_cast<uint8>(Value[Index++]);
			if (Lead < 0x80) continue;
			uint32 Code = 0;
			size_t Continuations = 0;
			if ((Lead & 0xe0) == 0xc0) { Code = Lead & 0x1f; Continuations = 1; }
			else if ((Lead & 0xf0) == 0xe0) { Code = Lead & 0x0f; Continuations = 2; }
			else if ((Lead & 0xf8) == 0xf0) { Code = Lead & 0x07; Continuations = 3; }
			else return false;
			if (Index + Continuations > Value.size()) return false;
			for (size_t Part = 0; Part < Continuations; ++Part)
			{
				const uint8 Next = static_cast<uint8>(Value[Index++]);
				if ((Next & 0xc0) != 0x80) return false;
				Code = (Code << 6) | (Next & 0x3f);
			}
			if ((Continuations == 1 && Code < 0x80) || (Continuations == 2 && Code < 0x800)
				|| (Continuations == 3 && Code < 0x10000) || Code > 0x10ffff
				|| (Code >= 0xd800 && Code <= 0xdfff)) return false;
		}
		return true;
	}

	inline auto ContainsHardReferenceType(const FSerializedType& Type) -> bool
	{
		return Type.Kind == EValueKind::HardReference
			|| std::ranges::any_of(Type.Children, ContainsHardReferenceType);
	}

	inline auto CanonicalFloat32(uint32 Bits) -> uint32
	{
		return (Bits & 0x7f800000u) == 0x7f800000u && (Bits & 0x007fffffu) != 0
			? 0x7fc00000u : Bits;
	}

	inline auto CanonicalFloat64(uint64 Bits) -> uint64
	{
		return (Bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull
			&& (Bits & 0x000fffffffffffffull) != 0 ? 0x7ff8000000000000ull : Bits;
	}

	inline auto IsValidTypeShape(EValueKind Kind, std::string_view Name, uint64 Parameter, size_t Children) -> bool
	{
		if (Kind > EValueKind::BulkData || Children > DastMaximumTableEntries) return false;
		switch (Kind)
		{
		case EValueKind::Enum: return !Name.empty() && Children == 0 && Parameter >= uint64(EValueKind::I8) && Parameter <= uint64(EValueKind::U64);
		case EValueKind::Intrinsic: return Children == 0 && Parameter >= 1 && Parameter <= 6;
		case EValueKind::Struct: return !Name.empty();
		case EValueKind::FixedArray: return Children == 1 && Parameter <= DastMaximumContainerElements;
		case EValueKind::Array: return Children == 1;
		case EValueKind::Map: return Children == 2;
		default: return Children == 0;
		}
	}
}
