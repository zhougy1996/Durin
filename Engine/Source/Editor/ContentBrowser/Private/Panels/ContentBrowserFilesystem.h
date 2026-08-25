#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Durin::Editor::ContentBrowser::Private::ContentBrowserFilesystem
{
	struct FPathProbe
	{
		std::filesystem::file_status Status{};
		std::error_code Error;

		auto Exists() const -> bool
		{
			return !Error && std::filesystem::exists(Status);
		}

		auto IsDirectory() const -> bool
		{
			return !Error && std::filesystem::is_directory(Status);
		}
	};

	// Keeps transient filesystem failures at the Content Browser model/operation boundary.
	inline auto Probe(const std::filesystem::path& Path) -> FPathProbe
	{
		FPathProbe Result;
		Result.Status = std::filesystem::status(Path, Result.Error);
		if (Result.Error == std::errc::no_such_file_or_directory
			|| Result.Error == std::errc::not_a_directory)
		{
			Result.Status = std::filesystem::file_status(
				std::filesystem::file_type::not_found);
			Result.Error.clear();
		}
		return Result;
	}

	inline auto NormalizePath(std::string_view Path) -> std::string
	{
		if (Path.empty()) return {};
		const std::filesystem::path Input(Path);
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(Input, Error);
		return (Error ? Input : Absolute).lexically_normal().generic_string();
	}
} // namespace Durin::Editor::ContentBrowser::Private::ContentBrowserFilesystem
