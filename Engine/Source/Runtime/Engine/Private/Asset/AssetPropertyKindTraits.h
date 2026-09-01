#pragma once

#include "DObject/DObjectGlobals.h"

namespace Durin::AssetPrivate
{
	// Byte-tool scalar payloads are copied at their reflected element width without semantic decoding.
	inline constexpr auto IsByteToolRawScalarKind(DurinCodeGen::EPropertyGenFlags Kind) -> bool
	{
		switch (Kind)
		{
		case DurinCodeGen::EPropertyGenFlags::Bool:
		case DurinCodeGen::EPropertyGenFlags::Int8:
		case DurinCodeGen::EPropertyGenFlags::Int16:
		case DurinCodeGen::EPropertyGenFlags::Int32:
		case DurinCodeGen::EPropertyGenFlags::Int64:
		case DurinCodeGen::EPropertyGenFlags::UInt8:
		case DurinCodeGen::EPropertyGenFlags::UInt16:
		case DurinCodeGen::EPropertyGenFlags::UInt32:
		case DurinCodeGen::EPropertyGenFlags::UInt64:
		case DurinCodeGen::EPropertyGenFlags::Float:
		case DurinCodeGen::EPropertyGenFlags::Double:
		case DurinCodeGen::EPropertyGenFlags::Enum:
		case DurinCodeGen::EPropertyGenFlags::Byte:
			return true;
		default:
			return false;
		}
	}
}
