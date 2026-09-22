#pragma once

#include <expected>

namespace Durin
{
	enum class EPlatformProcessError : uint8 { InvalidArguments, Launch, Wait, OpenPath };
	struct FPlatformProcessError
	{
		EPlatformProcessError Code;
		std::string Message;
		auto ToString() const -> const std::string& { return Message; }
	};

	struct FGenericPlatformProcess
	{
	};
}