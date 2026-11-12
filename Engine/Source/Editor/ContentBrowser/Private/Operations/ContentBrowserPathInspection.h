#pragma once
#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace Durin::ContentBrowser::Private
{
	inline auto IsReparsePoint(
			const std::filesystem::path& Path,
			std::error_code& OutError) -> bool
		{
			OutError.clear();
			const std::filesystem::file_status Status =
				std::filesystem::symlink_status(Path, OutError);
			if (OutError || std::filesystem::is_symlink(Status))
				return !OutError;
#ifdef _WIN32
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES)
			{
				OutError = std::error_code(
					static_cast<int>(GetLastError()), std::system_category());
				return false;
			}
			return (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			return false;
#endif
		}

	inline auto FindReparsePointInPath(
			const std::filesystem::path& Root,
			const std::filesystem::path& Candidate,
			std::filesystem::path& OutReparsePoint,
			std::error_code& OutError) -> bool
		{
			OutReparsePoint.clear();
			OutError.clear();
			std::filesystem::path Relative;
			if (!FPaths::TryMakeLexicalRelativePath(
					Candidate, Root, Relative))
			{
				OutError = std::make_error_code(std::errc::invalid_argument);
				return false;
			}
			std::filesystem::path Current = Root;
			for (const std::filesystem::path& Component : Relative)
			{
				Current /= Component;
				if (IsReparsePoint(Current, OutError))
				{
					OutReparsePoint = std::move(Current);
					return true;
				}
				if (OutError) return false;
			}
			return false;
		}

	inline auto AreSamePath(std::string_view A, std::string_view B) -> bool
		{
			std::filesystem::path Relative;
			return FPaths::TryMakeLexicalRelativePath(
				std::filesystem::path(A), std::filesystem::path(B), Relative)
				&& Relative.empty();
		}
}
