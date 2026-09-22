#pragma once

#include <expected>

#include "CoreAPI.h"

namespace Durin
{
	enum class EProjectError : uint8 { HistoryLoad, HistorySave, InvalidDescriptor, InvalidPaths, EditOwnership, Relaunch };
	struct FProjectError
	{
		EProjectError Code;
		std::string Message;
		auto ToString() const -> const std::string& { return Message; }
	};

}
