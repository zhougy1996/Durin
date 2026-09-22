#pragma once

#include <expected>

#include "CoreAPI.h"

namespace Durin
{
	enum class EStartupCommandError : uint8 { InvalidName, AlreadyPending, MissingHandler };
	struct FStartupCommandError
	{
		EStartupCommandError Code;
		std::string Message;
		auto ToString() const -> const std::string& { return Message; }
	};

	using FStartupCommandHandler = std::function<int(std::span<const std::string>)>;

	// Carries one bounded, opaque command through ordinary engine initialization.
	// Feature modules register handlers; Launch only admits and dispatches the envelope.
	[[nodiscard]] CORE_API auto ConfigureStartupCommand(
		std::string Name,
		std::vector<std::string> Arguments) -> std::expected<void, FStartupCommandError>;
	// Register, dispatch, and unregister on the control thread. Unregister before
	// destroying captures or unloading handler code; dispatch must have returned.
	CORE_API auto RegisterStartupCommandHandler(
		std::string Name,
		FStartupCommandHandler Handler) -> uint64;
	CORE_API auto UnregisterStartupCommandHandler(uint64 Handle) -> void;
	CORE_API auto HasPendingStartupCommand() -> bool;
	// Without bRequireHandler, a not-yet-loaded feature module leaves the command pending.
	// A successful empty result means no dispatch; a value is the handler's exit code.
	// A required missing handler consumes the command and returns an error.
	[[nodiscard]] CORE_API auto DispatchStartupCommand(
		bool bRequireHandler = false)
		-> std::expected<std::optional<int>, FStartupCommandError>;
}
