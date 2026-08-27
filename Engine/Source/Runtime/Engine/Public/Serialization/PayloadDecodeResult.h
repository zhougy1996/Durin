#pragma once

#include <string>

#include "EngineAPI.h"

namespace Durin
{
	enum class EPayloadDecodeError
	{
		None,
		Incompatible,
		Corrupt
	};

	struct FPayloadDecodeResult
	{
		EPayloadDecodeError Code = EPayloadDecodeError::None;
		std::string Message;

		explicit operator bool() const { return Code == EPayloadDecodeError::None; }
	};
}
