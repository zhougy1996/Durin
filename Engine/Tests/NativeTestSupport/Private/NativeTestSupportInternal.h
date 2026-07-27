#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Durin::Testing::Private
{
	using FNonceGenerator = std::function<std::string()>;

	[[nodiscard]] auto CreateUniqueRunDirectory(
		const std::filesystem::path& WorkRoot,
		std::uint32_t ProcessId,
		const FNonceGenerator& NonceGenerator) -> std::filesystem::path;
}
