#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Durin::Tests::ProcessFixture
{
	inline constexpr std::array EnvironmentNames = {
		"DURIN_PROCESS_INHERITED", "DURIN_PROCESS_OVERRIDE", "DURIN_PROCESS_REMOVE",
		"DURIN_PROCESS_EMPTY", "DURIN_PROCESS_UNICODE", "DURIN_PROCESS_CASE", "durin_process_case",
		"DURIN_\xe6\xb5\x8b\xe8\xaf\x95"};

	struct FReport
	{
		uint32_t ProcessId = 0;
		std::vector<std::string> Arguments;
		std::string WorkingDirectory;
		std::array<std::optional<std::string>, EnvironmentNames.size()> Environment;
	};

	inline auto WriteReport(const std::filesystem::path& Path, const FReport& Report) -> bool
	{
		std::ofstream Output(Path, std::ios::binary);
		Output << Report.ProcessId << '\n' << Report.Arguments.size() << '\n';
		for (const auto& Argument : Report.Arguments) Output << std::quoted(Argument) << '\n';
		Output << std::quoted(Report.WorkingDirectory) << '\n';
		for (const auto& Value : Report.Environment)
			Output << Value.has_value() << ' ' << std::quoted(Value.value_or("")) << '\n';
		Output << "complete\n";
		Output.close();
		return !Output.fail();
	}

	inline auto ReadReport(const std::filesystem::path& Path) -> std::optional<FReport>
	{
		std::ifstream Input(Path, std::ios::binary);
		FReport Report;
		size_t Count = 0;
		if (!(Input >> Report.ProcessId >> Count) || Count > 1024) return {};
		Report.Arguments.resize(Count);
		for (auto& Argument : Report.Arguments) Input >> std::quoted(Argument);
		Input >> std::quoted(Report.WorkingDirectory);
		for (auto& Value : Report.Environment)
		{
			bool Present = false;
			std::string Text;
			Input >> Present >> std::quoted(Text);
			if (Present) Value = std::move(Text);
		}
		std::string Complete;
		if (!(Input >> Complete) || Complete != "complete") return {};
		return Report;
	}
}
