#pragma once

#include "CoreMinimal.h"
#include "Console/ConsoleCommand.h"
#include "Tools/Tool.h"

namespace Durin::Editor::MainFrame
{
	inline constexpr size_t MaxConsoleToolTextBytes = 1024 * 1024;
	// The structured tokenizer preserves Windows paths and does not consume inline JSON.
	auto TokenizeToolText(std::string_view Line) -> std::expected<std::vector<std::string>, std::string>;
	auto ParseConsoleToolArguments(const FToolDescriptor& Descriptor, std::span<const std::string> Tokens)
		-> std::expected<FToolValue, std::string>;
	auto FormatConsoleToolResult(const FToolResult& Result) -> FConsoleCommandResult;
}
