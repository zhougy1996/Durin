#pragma once

#include <filesystem>

namespace Durin::Testing
{
	[[nodiscard]] auto GetTestWorkDirectory() -> const std::filesystem::path&;
	[[nodiscard]] auto CreateTestWorkSubdirectory(
		const std::filesystem::path& RelativePath) -> std::filesystem::path;
	[[nodiscard]] auto IsTestWorkDirectoryKept() -> bool;

	auto RunNativeTests(
		int ArgumentCount,
		char** Arguments,
		const std::filesystem::path& WorkRoot) -> int;
}
