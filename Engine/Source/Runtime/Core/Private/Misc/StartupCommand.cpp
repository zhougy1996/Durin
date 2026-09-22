#include "Misc/StartupCommand.h"

namespace Durin
{
	namespace
	{
		struct FPendingStartupCommand
		{
			std::string Name;
			std::vector<std::string> Arguments;
		};

		struct FRegisteredStartupCommand
		{
			uint64 Handle = 0;
			FStartupCommandHandler Handler;
		};

		std::optional<FPendingStartupCommand> GPendingStartupCommand;
		std::unordered_map<std::string, FRegisteredStartupCommand> GStartupCommandHandlers;
		uint64 GNextStartupCommandHandle = 1;
	}

	auto ConfigureStartupCommand(
		std::string Name,
		std::vector<std::string> Arguments) -> std::expected<void, FStartupCommandError>
	{
		if (Name.empty())
		{
			return std::unexpected(FStartupCommandError{EStartupCommandError::InvalidName, "The startup command name cannot be empty."});
		}
		if (GPendingStartupCommand)
		{
			return std::unexpected(FStartupCommandError{EStartupCommandError::AlreadyPending, "Only one startup command is allowed per process."});
		}
		GPendingStartupCommand = FPendingStartupCommand{
			std::move(Name), std::move(Arguments)};
		return {};
	}

	auto RegisterStartupCommandHandler(
		std::string Name,
		FStartupCommandHandler Handler) -> uint64
	{
		if (Name.empty() || !Handler
			|| GStartupCommandHandlers.contains(Name))
			return 0;
		const uint64 Handle = GNextStartupCommandHandle++;
		GStartupCommandHandlers.emplace(
			std::move(Name), FRegisteredStartupCommand{
				Handle, std::move(Handler)});
		return Handle;
	}

	auto UnregisterStartupCommandHandler(uint64 Handle) -> void
	{
		if (!Handle) return;
		std::erase_if(GStartupCommandHandlers, [Handle](const auto& Entry) {
			return Entry.second.Handle == Handle;
		});
	}

	auto HasPendingStartupCommand() -> bool
	{
		return GPendingStartupCommand.has_value();
	}

	auto DispatchStartupCommand(bool bRequireHandler)
		-> std::expected<std::optional<int>, FStartupCommandError>
	{
		if (!GPendingStartupCommand) return std::nullopt;
		const auto It = GStartupCommandHandlers.find(GPendingStartupCommand->Name);
		if (It == GStartupCommandHandlers.end())
		{
			if (!bRequireHandler) return std::nullopt;
			const std::string Message = std::format(
					"No initialized module handles startup command '{}'.",
					GPendingStartupCommand->Name);
			GPendingStartupCommand.reset();
			return std::unexpected(FStartupCommandError{EStartupCommandError::MissingHandler, Message});
		}
		FPendingStartupCommand Command = std::move(*GPendingStartupCommand);
		GPendingStartupCommand.reset();
		return It->second.Handler(Command.Arguments);
	}
}
