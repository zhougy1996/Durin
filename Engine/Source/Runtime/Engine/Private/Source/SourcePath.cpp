#include "Source/SourcePath.h"

#include "Misc/Paths.h"

namespace Durin
{
	auto TryMigrateLegacySourcePath(
		std::string_view PackagePath,
		std::string_view LegacyPath,
		FSourcePath& OutSourcePath,
		std::filesystem::path& OutPhysicalPath,
		std::string& OutError) -> bool
	{
		OutSourcePath = {};
		OutPhysicalPath.clear();
		const PathUtilities::FMountLookupResult Owner =
			PathUtilities::FindMountForVirtualPath(PackagePath);
		if (!Owner)
		{
			OutError = std::format(
				"Legacy source owner '{}' is invalid: {}", PackagePath, Owner.Message);
			return false;
		}

		const std::filesystem::path Legacy(LegacyPath);
		const std::filesystem::path Normalized = Legacy.lexically_normal();
		const std::string NormalizedString = Normalized.generic_string();
		const bool bContainsParent = std::ranges::any_of(
			Legacy, [](const std::filesystem::path& Part) { return Part == ".."; });
		if (LegacyPath.empty() || Legacy.is_absolute() || LegacyPath.starts_with('/')
			|| LegacyPath.find('\\') != std::string_view::npos || bContainsParent
			|| NormalizedString != LegacyPath || !NormalizedString.starts_with("SourceAssets/"))
		{
			OutError = std::format(
				"Legacy source path '{}' is not a normalized SourceAssets-relative file path.",
				LegacyPath);
			return false;
		}

		const std::filesystem::path Relative = Normalized.lexically_relative("SourceAssets");
		if (Relative.empty() || Relative == ".")
		{
			OutError = "Legacy source path does not identify a file beneath SourceAssets.";
			return false;
		}
		const std::string VirtualPath = Owner.Mount->VirtualRoot + Relative.generic_string();
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				VirtualPath, PathUtilities::EPathExistence::RequireFile);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		OutSourcePath.Path = Resolved.NormalizedVirtualPath;
		OutPhysicalPath = Resolved.PhysicalPath;
		OutError.clear();
		return true;
	}
}
