#pragma once

#include "CoreAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	using FStartupCommandHandler = std::function<int(std::span<const std::string>)>;

	// Carries one bounded, opaque command through ordinary engine initialization.
	// Feature modules register handlers; Launch only admits and dispatches the envelope.
	CORE_API auto ConfigureStartupCommand(
		std::string Name,
		std::vector<std::string> Arguments,
		std::string* OutError = nullptr) -> bool;
	CORE_API auto RegisterStartupCommandHandler(
		std::string Name,
		FStartupCommandHandler Handler,
		FModuleOwnedCallbackGate OwnerGate) -> uint64;
	CORE_API auto UnregisterStartupCommandHandler(uint64 Handle) -> void;
	CORE_API auto HasPendingStartupCommand() -> bool;
	// Without bRequireHandler, a not-yet-loaded feature module leaves the command pending.
	CORE_API auto DispatchStartupCommand(
		std::string* OutError = nullptr,
		bool bRequireHandler = false)
		-> std::optional<int>;
}
