#pragma once

#include "Misc/CoreTypes.h"

#include "Misc/CoreStd.h"

namespace Durin
{
	// Owned child configuration. Arguments and environment strings are UTF-8;
	// no shell parsing, quoting, expansion or implicit argv[0] is performed by callers.
	struct FProcessLaunchOptions
	{
		std::vector<std::string> Arguments;
		// Empty inherits the parent's directory; otherwise requires an absolute native path.
		std::filesystem::path WorkingDirectory;
		// Inherit the parent environment, then set values or remove nullopt entries.
		// Empty strings remain present. Names exclude '=' and NUL; values exclude NUL.
		// Names are case-insensitive on Windows (case aliases are rejected), sensitive on macOS.
		std::map<std::string, std::optional<std::string>> EnvironmentOverrides;
	};

    // Synchronous owned execution. Callbacks run on the caller thread; output is
    // bounded chunks, not lines. Cancellation input allows a protocol-aware child
    // to stop gracefully; otherwise the owned tree is stopped immediately.
    struct FProcessExecutionControl
    {
        std::string StandardInput, CancellationInput;
        std::function<bool()> IsCancelled;
        std::function<void(std::string_view)> OnOutput;
    };

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
