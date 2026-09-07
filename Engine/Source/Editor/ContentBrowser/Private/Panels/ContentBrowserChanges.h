#pragma once

#include "AssetRegistry/ContentChanges.h"
#include "Misc/Paths.h"

namespace Durin::Editor::ContentBrowser::Private::ContentBrowserChanges
{
	// Virtual asset paths have slash-delimited, case-sensitive identities on every host.
	inline auto AssetDirectory(std::string_view Directory) -> std::string_view
	{
		while (Directory.size() > 1 && Directory.ends_with('/')) Directory.remove_suffix(1);
		return Directory;
	}
	inline auto WithinAssetDirectory(std::string_view Path, std::string_view Directory) -> bool
	{
		Directory = AssetDirectory(Directory);
		return !Directory.empty() && !Path.empty() && (Path == Directory
			|| (Path.starts_with(Directory) && (Directory == "/"
				|| (Path.size() > Directory.size() && Path[Directory.size()] == '/'))));
	}
	inline auto SamePath(std::string_view A, std::string_view B) -> bool
	{
		if (A.empty() || B.empty()) return false;
		std::filesystem::path Relative;
		return FPaths::TryMakeLexicalRelativePath(std::filesystem::path(A), std::filesystem::path(B), Relative)
			&& Relative.empty();
	}
	inline auto Within(std::string_view Path, std::string_view Directory, bool Recursive = true) -> bool
	{
		return !Path.empty() && !Directory.empty() && (SamePath(Path, Directory)
			|| FPaths::IsLexicalDescendantPath(std::filesystem::path(Path), std::filesystem::path(Directory), Recursive));
	}
	// An asset addition can introduce directories even when its producer only knows the file.
	inline auto IntroducedChild(const FContentChange& Change, std::string_view Directory) -> std::string
	{
		if ((Change.Kind != EContentChangeKind::Added && Change.Kind != EContentChangeKind::Renamed)
			|| Change.NewPhysicalPath.empty()) return {};
		const auto Parent = Change.bDirectory ? std::filesystem::path(Change.NewPhysicalPath)
			: std::filesystem::path(Change.NewPhysicalPath).parent_path();
		if (SamePath(Parent.generic_string(), Directory) || !Within(Parent.generic_string(), Directory)) return {};
		std::filesystem::path Relative;
		if (!FPaths::TryMakeLexicalRelativePath(Parent, std::filesystem::path(Directory), Relative) || Relative.empty()) return {};
		return (std::filesystem::path(Directory) / *Relative.begin()).generic_string();
	}
	inline auto Affects(const FContentChange& Change, std::string_view Directory, bool Recursive) -> bool
	{
		for (const auto* Path : {&Change.OldPhysicalPath, &Change.NewPhysicalPath})
			if (Within(*Path, Directory, Recursive)
				|| (Change.bDirectory && Within(Directory, *Path))) return true;
		return false;
	}
	inline auto RemapPhysical(std::string& Path, const FContentChange& Change) -> bool
	{
		if (Change.Kind != EContentChangeKind::Renamed || Change.NewPhysicalPath.empty()) return false;
		if (!SamePath(Path, Change.OldPhysicalPath)
			&& !(Change.bDirectory && Within(Path, Change.OldPhysicalPath))) return false;
		std::filesystem::path Relative;
		if (!FPaths::TryMakeLexicalRelativePath(std::filesystem::path(Path),
			std::filesystem::path(Change.OldPhysicalPath), Relative)) return false;
		Path = (std::filesystem::path(Change.NewPhysicalPath) / Relative).lexically_normal().generic_string();
		if (Path.ends_with('/')) Path.pop_back();
		return true;
	}
	inline auto MatchesAsset(std::string_view Identity, std::string_view PackageOrAsset) -> bool
	{
		return !PackageOrAsset.empty() && (Identity == PackageOrAsset
			|| (Identity.size() > PackageOrAsset.size() && Identity.starts_with(PackageOrAsset)
				&& Identity[PackageOrAsset.size()] == '.'));
	}
}
