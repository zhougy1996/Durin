#pragma once

#include <string>

namespace Durin
{
	enum class EDecodeError
	{
		None,
		Incompatible,
		Corrupt
	};

	struct FDecodeResult
	{
		EDecodeError Code = EDecodeError::None;
		std::string Message;

		explicit operator bool() const { return Code == EDecodeError::None; }
	};
}
