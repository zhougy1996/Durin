#include "Console/ConsoleCommand.h"

namespace Durin
{
	namespace
	{
		auto ToLower(std::string_view Text) -> std::string
		{
			std::string Result(Text);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Result;
		}

		auto ParseCommandLine(std::string_view Line, std::vector<std::string>& Tokens) -> FConsoleCommandResult
		{
			std::string Token;
			char Quote = 0;
			bool bEscaped = false;
			bool bTokenStarted = false;
			for (const char Character : Line)
			{
				if (bEscaped)
				{
					Token.push_back(Character);
					bEscaped = false;
					bTokenStarted = true;
				}
				else if (Character == '\\')
				{
					bEscaped = true;
					bTokenStarted = true;
				}
				else if (Quote != 0)
				{
					if (Character == Quote) Quote = 0;
					else Token.push_back(Character);
				}
				else if (Character == '\'' || Character == '"')
				{
					Quote = Character;
					bTokenStarted = true;
				}
				else if (std::isspace(static_cast<unsigned char>(Character)))
				{
					if (bTokenStarted)
					{
						Tokens.push_back(std::move(Token));
						Token.clear();
						bTokenStarted = false;
					}
				}
				else
				{
					Token.push_back(Character);
					bTokenStarted = true;
				}
			}
			if (bEscaped) return FConsoleCommandResult::Failure("Command ends with an incomplete escape sequence.");
			if (Quote != 0) return FConsoleCommandResult::Failure("Command contains an unterminated quote.");
			if (bTokenStarted) Tokens.push_back(std::move(Token));
			return FConsoleCommandResult::Success();
		}
	} // namespace

	FConsoleCommandRegistry::FConsoleCommandRegistry()
	{
		RegisterCommand({
			"help", "Lists commands or displays help for one command.", "help [command]",
			[this](std::span<const std::string> Args) {
				if (Args.size() > 1) return FConsoleCommandResult::Failure("Usage: help [command]");
				const std::vector<FConsoleCommandDesc> AvailableCommands = GetCommands();
				if (Args.empty())
				{
					std::string Message = "Available commands:";
					for (const auto& Command : AvailableCommands) Message += std::format("\n  {:<16} {}", Command.Name, Command.Description);
					return FConsoleCommandResult::Success(std::move(Message));
				}
				const std::string Name = ToLower(Args.front());
				const auto It = std::ranges::find_if(AvailableCommands, [&Name](const auto& Command) { return ToLower(Command.Name) == Name; });
				if (It == AvailableCommands.end()) return FConsoleCommandResult::Failure(std::format("Unknown command '{}'.", Args.front()));
				return FConsoleCommandResult::Success(std::format("{}\nUsage: {}", It->Description, It->Usage.empty() ? It->Name : It->Usage));
			}
		});
	}

	auto FConsoleCommandRegistry::Get() -> FConsoleCommandRegistry&
	{
		static FConsoleCommandRegistry Registry;
		return Registry;
	}

	auto FConsoleCommandRegistry::RegisterCommand(FConsoleCommandDesc Desc) -> FConsoleCommandHandle
	{
		if (Desc.Name.empty() || !Desc.Execute || std::ranges::any_of(Desc.Name, [](unsigned char C) { return std::isspace(C); })) return 0;
		const std::string Key = ToLower(Desc.Name);
		std::scoped_lock Lock(Mutex);
		if (Commands.contains(Key)) return 0;
		const FConsoleCommandHandle Handle = NextHandle.fetch_add(1, std::memory_order_relaxed);
		Commands.emplace(Key, std::pair{Handle, std::move(Desc)});
		return Handle;
	}

	auto FConsoleCommandRegistry::UnregisterCommand(FConsoleCommandHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::scoped_lock Lock(Mutex);
		std::erase_if(Commands, [Handle](const auto& Entry) { return Entry.second.first == Handle; });
	}

	auto FConsoleCommandRegistry::Execute(std::string_view CommandLine) const -> FConsoleCommandResult
	{
		std::vector<std::string> Tokens;
		if (FConsoleCommandResult ParseResult = ParseCommandLine(CommandLine, Tokens); !ParseResult.bSuccess) return ParseResult;
		if (Tokens.empty()) return FConsoleCommandResult::Success();
		FConsoleCommandDesc Command;
		{
			std::scoped_lock Lock(Mutex);
			const auto It = Commands.find(ToLower(Tokens.front()));
			if (It == Commands.end()) return FConsoleCommandResult::Failure(std::format("Unknown command '{}'. Type 'help' for a list of commands.", Tokens.front()));
			Command = It->second.second;
		}
		try
		{
			return Command.Execute(std::span<const std::string>(Tokens).subspan(1));
		}
		catch (const std::exception& Error)
		{
			return FConsoleCommandResult::Failure(std::format("Command '{}' failed: {}", Command.Name, Error.what()));
		}
		catch (...)
		{
			return FConsoleCommandResult::Failure(std::format("Command '{}' failed with an unknown error.", Command.Name));
		}
	}

	auto FConsoleCommandRegistry::FindCompletions(std::string_view Prefix) const -> std::vector<std::string>
	{
		const std::string Key = ToLower(Prefix);
		std::vector<std::string> Result;
		std::scoped_lock Lock(Mutex);
		for (const auto& [Name, Entry] : Commands) if (Name.starts_with(Key)) Result.push_back(Entry.second.Name);
		std::ranges::sort(Result, {}, [](const std::string& Name) { return ToLower(Name); });
		return Result;
	}

	auto FConsoleCommandRegistry::GetCommands() const -> std::vector<FConsoleCommandDesc>
	{
		std::vector<FConsoleCommandDesc> Result;
		std::scoped_lock Lock(Mutex);
		for (const auto& [Name, Entry] : Commands) Result.push_back(Entry.second);
		std::ranges::sort(Result, {}, [](const FConsoleCommandDesc& Desc) { return ToLower(Desc.Name); });
		return Result;
	}
} // namespace Durin
