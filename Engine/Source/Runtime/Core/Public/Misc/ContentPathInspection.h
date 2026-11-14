#pragma once
#include "CoreMinimal.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/StringHelper.h"
#include "Misc/Paths.h"

namespace Durin::ContentStorage
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

namespace Durin::ContentStorage::TransferPaths
{
	inline auto Same(const std::filesystem::path& A, const std::filesystem::path& B) -> bool
	{
		std::filesystem::path Relative;
		return FPaths::TryMakeLexicalRelativePath(A, B, Relative) && Relative.empty();
	}
	inline auto Claim(const std::filesystem::path& Path) -> std::string
	{
		auto Key = std::filesystem::absolute(Path).lexically_normal().generic_string();
#ifdef _WIN32
		Key = StringUtils::FoldAscii(Key);
#endif
		return "NativeFilesystem:" + Key;
	}
	inline auto SafePath(const std::filesystem::path& Path, bool Writable, std::string* Error) -> bool
	{
		const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Path.generic_string());
		if (!Mount || (Writable && !Mount.Mount->bContentWritable))
		{ if (Error) *Error = "Transfer requires an admitted content mount."; return false; }
		std::filesystem::path Relative;
		if (!FPaths::TryMakeLexicalRelativePath(Path, Mount.Mount->GetContentDir(), Relative)) return false;
		auto Current = Mount.Mount->GetContentDir();
		auto Inspect = [&](const std::filesystem::path& Candidate) {
			std::error_code Ec;
			const auto Status = std::filesystem::symlink_status(Candidate, Ec);
			if (Ec == std::errc::no_such_file_or_directory) return true;
			if (Ec || std::filesystem::is_symlink(Status)) return false;
#ifdef _WIN32
			if (std::filesystem::exists(Status))
			{
				const DWORD Flags = GetFileAttributesW(Candidate.c_str());
				if (Flags == INVALID_FILE_ATTRIBUTES || (Flags & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
			}
#endif
			return true;
		};
		if (!Inspect(Current)) { if (Error) *Error = "Transfer cannot traverse a reparse point."; return false; }
		for (const auto& Component : Relative)
		{
			Current /= Component;
			if (!Inspect(Current)) { if (Error) *Error = "Transfer cannot traverse a reparse point or unreadable path."; return false; }
		}
		return true;
	}
}
