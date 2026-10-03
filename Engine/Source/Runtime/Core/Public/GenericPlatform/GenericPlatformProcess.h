#pragma once

#include "Misc/CoreTypes.h"

#include "Misc/CoreStd.h"

namespace Durin
{
	enum class EPlatformProcessError : uint8 { InvalidArguments, Launch, Wait, OpenPath };
	struct FPlatformProcessError
	{
		EPlatformProcessError Code;
		std::string Message;
		std::string Path;
		std::optional<int64> NativeError;
		std::optional<int32> ExitCode;
		auto ToString() const -> const std::string& { return Message; }
	};

	struct FGenericPlatformProcess
	{
	};
}