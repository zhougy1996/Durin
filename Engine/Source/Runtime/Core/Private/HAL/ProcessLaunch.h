#pragma once

#include "GenericPlatform/GenericPlatformProcess.h"

namespace Durin::Detail
{
	inline auto IsProcessUtf8(std::string_view Text) -> bool
	{
		for (size_t Index = 0; Index < Text.size();)
		{
			const auto Lead = static_cast<unsigned char>(Text[Index++]);
			if (Lead == 0) return false;
			if (Lead < 0x80) continue;
			const size_t Continuations = Lead >= 0xc2 && Lead <= 0xdf ? 1
				: Lead >= 0xe0 && Lead <= 0xef ? 2 : Lead >= 0xf0 && Lead <= 0xf4 ? 3 : 0;
			if (Continuations == 0 || Text.size() - Index < Continuations) return false;
			uint32 Codepoint = Lead & ((1u << (6 - Continuations)) - 1);
			for (size_t Offset = 0; Offset < Continuations; ++Offset)
			{
				const auto Byte = static_cast<unsigned char>(Text[Index++]);
				if ((Byte & 0xc0) != 0x80) return false;
				Codepoint = (Codepoint << 6) | (Byte & 0x3f);
			}
			const uint32 Minimum = Continuations == 1 ? 0x80 : Continuations == 2 ? 0x800 : 0x10000;
			if (Codepoint < Minimum || Codepoint > 0x10ffff || (Codepoint >= 0xd800 && Codepoint <= 0xdfff)) return false;
		}
		return true;
	}

	inline auto InvalidProcessLaunch(std::string_view Executable, std::string Message) -> FPlatformProcessError
	{
		return {EPlatformProcessError::InvalidArguments, std::move(Message), std::string(Executable)};
	}

	inline auto ValidateProcessLaunch(std::string_view Executable, const FProcessLaunchOptions& Options)
		-> std::expected<void, FPlatformProcessError>
	{
		if (Executable.empty() || !IsProcessUtf8(Executable))
			return std::unexpected(InvalidProcessLaunch(Executable, "Executable must be nonempty UTF-8 without NULs."));
		if (!std::filesystem::path(std::u8string(Executable.begin(), Executable.end())).is_absolute())
			return std::unexpected(InvalidProcessLaunch(Executable, "Executable must be an absolute path; PATH search is not performed."));
		for (const auto& Argument : Options.Arguments)
			if (!IsProcessUtf8(Argument))
				return std::unexpected(InvalidProcessLaunch(Executable, "Arguments must be UTF-8 without NULs."));
		const auto& Directory = Options.WorkingDirectory;
		if (!Directory.empty() && (!Directory.is_absolute()
			|| Directory.native().find(std::filesystem::path::value_type{}) != Directory.native().npos))
			return std::unexpected(InvalidProcessLaunch(Executable, "Working directory must be absolute and contain no NULs."));
		for (const auto& [Name, Value] : Options.EnvironmentOverrides)
			if (Name.empty() || Name.find('=') != Name.npos || !IsProcessUtf8(Name) || (Value && !IsProcessUtf8(*Value)))
				return std::unexpected(InvalidProcessLaunch(Executable, "Environment overrides require nonempty UTF-8 names without '=' or NULs and UTF-8 values without NULs."));
		return {};
	}
}
