#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	// Stores an optional diagnostic and returns false for compact validation paths.
	template<typename MessageType>
	requires std::constructible_from<std::string, MessageType&&>
	[[nodiscard]] inline auto Fail(std::string* OutError, MessageType&& Message) -> bool
	{
		if (OutError) *OutError = std::string(std::forward<MessageType>(Message));
		return false;
	}

	template<typename MessageType>
	requires std::constructible_from<std::string, MessageType&&>
	[[nodiscard]] inline auto Fail(MessageType&& Message, std::string* OutError) -> bool
	{
		return Fail(OutError, std::forward<MessageType>(Message));
	}

	template<typename MessageType>
	requires std::constructible_from<std::string, MessageType&&>
	[[nodiscard]] inline auto Fail(std::string& OutError, MessageType&& Message) -> bool
	{
		OutError = std::string(std::forward<MessageType>(Message));
		return false;
	}
}
