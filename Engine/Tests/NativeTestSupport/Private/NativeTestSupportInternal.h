#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Durin::Testing::Private
{
	using FNonceGenerator = std::function<std::string()>;
	using FProcessRunningPredicate = std::function<bool(std::uint32_t)>;

	[[nodiscard]] auto CreateUniqueRunDirectory(
		const std::filesystem::path& WorkRoot,
		std::uint32_t ProcessId,
		const FNonceGenerator& NonceGenerator) -> std::filesystem::path;

	auto CleanupAbandonedSuccessfulRunDirectories(
		const std::filesystem::path& WorkRoot,
		std::uint32_t CurrentProcessId,
		std::filesystem::file_time_type CurrentTime,
		std::filesystem::file_time_type::duration MinimumAge,
		const FProcessRunningPredicate& IsProcessRunning) -> std::uintmax_t;
}
