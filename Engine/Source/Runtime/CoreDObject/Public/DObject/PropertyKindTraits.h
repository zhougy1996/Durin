#pragma once

#include "DObject/DObjectGlobals.h"

#include <array>
#include <cstddef>

namespace Durin::DurinCodeGen
{
	inline constexpr std::array AllPropertyKinds{
		EPropertyGenFlags::None,
		EPropertyGenFlags::Bool,
		EPropertyGenFlags::Int8,
		EPropertyGenFlags::Int16,
		EPropertyGenFlags::Int32,
		EPropertyGenFlags::Int64,
		EPropertyGenFlags::UInt8,
		EPropertyGenFlags::UInt16,
		EPropertyGenFlags::UInt32,
		EPropertyGenFlags::UInt64,
		EPropertyGenFlags::Float,
		EPropertyGenFlags::Double,
		EPropertyGenFlags::String,
		EPropertyGenFlags::Enum,
		EPropertyGenFlags::Object,
		EPropertyGenFlags::Array,
		EPropertyGenFlags::Map,
		EPropertyGenFlags::Struct,
		EPropertyGenFlags::Name,
		EPropertyGenFlags::Guid,
		EPropertyGenFlags::SoftObject,
		EPropertyGenFlags::WeakObject,
		EPropertyGenFlags::Byte,
		EPropertyGenFlags::Blob,
		EPropertyGenFlags::BulkData,
	};

	static_assert(AllPropertyKinds.size() == static_cast<size_t>(EPropertyGenFlags::Count));
	static_assert([] {
		for (size_t Index = 0; Index < AllPropertyKinds.size(); ++Index)
		{
			if (static_cast<size_t>(AllPropertyKinds[Index]) != Index) return false;
		}
		return true;
	}(), "AllPropertyKinds must list every property kind once in declaration order.");

	constexpr auto IsSignedIntegralKind(EPropertyGenFlags Kind) -> bool
	{
		switch (Kind)
		{
		case EPropertyGenFlags::Int8:
		case EPropertyGenFlags::Int16:
		case EPropertyGenFlags::Int32:
		case EPropertyGenFlags::Int64:
			return true;
		default:
			return false;
		}
	}

	constexpr auto IsUnsignedIntegralKind(EPropertyGenFlags Kind) -> bool
	{
		switch (Kind)
		{
		case EPropertyGenFlags::UInt8:
		case EPropertyGenFlags::UInt16:
		case EPropertyGenFlags::UInt32:
		case EPropertyGenFlags::UInt64:
			return true;
		default:
			return false;
		}
	}

	constexpr auto IsIntegralKind(EPropertyGenFlags Kind) -> bool
	{
		return IsSignedIntegralKind(Kind) || IsUnsignedIntegralKind(Kind);
	}

	constexpr auto IsFloatingPointKind(EPropertyGenFlags Kind) -> bool
	{
		return Kind == EPropertyGenFlags::Float || Kind == EPropertyGenFlags::Double;
	}

	// Numeric kinds support arithmetic metadata; Bool, Enum, and Byte retain distinct semantics.
	constexpr auto IsNumericKind(EPropertyGenFlags Kind) -> bool
	{
		return IsIntegralKind(Kind) || IsFloatingPointKind(Kind);
	}

	// Fixed-width scalar kinds use inline value storage; Enum width comes from its property descriptor.
	constexpr auto IsFixedWidthScalarKind(EPropertyGenFlags Kind) -> bool
	{
		return Kind == EPropertyGenFlags::Bool
			|| IsNumericKind(Kind)
			|| Kind == EPropertyGenFlags::Enum
			|| Kind == EPropertyGenFlags::Byte;
	}

	// Bitwise identity compares the complete stored representation, including floating-point payload bits.
	constexpr auto IsBitwiseIdentityKind(EPropertyGenFlags Kind) -> bool
	{
		switch (Kind)
		{
		case EPropertyGenFlags::Bool:
		case EPropertyGenFlags::Int8:
		case EPropertyGenFlags::Int16:
		case EPropertyGenFlags::Int32:
		case EPropertyGenFlags::Int64:
		case EPropertyGenFlags::UInt8:
		case EPropertyGenFlags::UInt16:
		case EPropertyGenFlags::UInt32:
		case EPropertyGenFlags::UInt64:
		case EPropertyGenFlags::Float:
		case EPropertyGenFlags::Double:
		case EPropertyGenFlags::Enum:
		case EPropertyGenFlags::Byte:
			return true;
		default:
			return false;
		}
	}
}
