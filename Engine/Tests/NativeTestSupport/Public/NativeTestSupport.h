#pragma once

#include <filesystem>

namespace Durin::Testing
{
	[[nodiscard]] auto GetTestWorkDirectory() -> const std::filesystem::path&;
	[[nodiscard]] auto CreateTestWorkSubdirectory(
		const std::filesystem::path& RelativePath) -> std::filesystem::path;
	[[nodiscard]] auto CreateTestFixtureDirectory(
		const std::filesystem::path& RelativePath) -> std::filesystem::path;
	auto RemoveTestWorkDirectory(
		const std::filesystem::path& Path) -> std::uintmax_t;
	auto RemoveTestWorkDirectory(
		const std::filesystem::path& Path,
		std::error_code& ErrorCode) noexcept -> std::uintmax_t;
	[[nodiscard]] auto IsTestWorkDirectoryKept() -> bool;

	auto RunNativeTests(
		int ArgumentCount,
		char** Arguments,
		const std::filesystem::path& WorkRoot) -> int;
}
