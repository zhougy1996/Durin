#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	// Stores an optional diagnostic and returns false for compact validation paths.
	inline auto Fail(std::string_view Message, std::string* OutError = nullptr) -> bool
	{
		if (OutError) OutError->assign(Message);
		return false;
	}
}
