#pragma once

#include "CoreAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	using FConsoleCommandHandle = uint64;

	// Reports command success together with optional user-facing output.
	struct FConsoleCommandResult
	{
		bool bSuccess = true;
		std::string Message;

		static auto Success(std::string Message = {}) -> FConsoleCommandResult { return {true, std::move(Message)}; }
		static auto Failure(std::string Message) -> FConsoleCommandResult { return {false, std::move(Message)}; }
	};

	// Describes a registered command and the callback that executes it.
	struct FConsoleCommandDesc
	{
		std::string Name;
		std::string Description;
		std::string Usage;
		std::function<FConsoleCommandResult(std::span<const std::string>)> Execute;
	};

	// Owns the process-wide, thread-safe console command registry.
	class FConsoleCommandRegistry
	{
	public:
		CORE_API FConsoleCommandRegistry();

		CORE_API static auto Get() -> FConsoleCommandRegistry&;
		CORE_API auto RegisterCommand(
			FConsoleCommandDesc Desc,
			FModuleOwnedCallbackGate OwnerGate = {}) -> FConsoleCommandHandle;
		CORE_API auto UnregisterCommand(FConsoleCommandHandle Handle) -> void;
		CORE_API auto Execute(std::string_view CommandLine) const -> FConsoleCommandResult;
		CORE_API auto FindCompletions(std::string_view Prefix) const -> std::vector<std::string>;
		CORE_API auto GetCommands() const -> std::vector<FConsoleCommandDesc>;

	private:
		struct FEntry
		{
			FConsoleCommandHandle Handle = 0;
			FModuleOwnedResourceLease OwnerResource;
			FConsoleCommandDesc Desc;
			FModuleOwnedCallbackGate OwnerGate;
		};
		mutable std::mutex Mutex;
		std::unordered_map<std::string, FEntry> Commands;
		std::atomic<FConsoleCommandHandle> NextHandle = 1;
	};
} // namespace Durin
