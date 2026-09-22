#pragma once

#include <expected>
#include <optional>

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