#include "Panels/ContentBrowserItemDiscovery.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"

namespace Durin::ContentBrowser::Private
{
	using ContentBrowserFilesystem::NormalizePath;

	auto FContentBrowserItemDiscovery::CaptureItems(
		const std::string& PhysicalDirectory,
		bool bRecursive, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
		const Tasks::FCancellationToken& Cancellation)
		-> FContentBrowserItemsSnapshot
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureItems");
		Result = {};
		if (PhysicalDirectory.empty()) return std::move(Result);
		auto AppendFilesystemEntry =
			[&](const std::filesystem::directory_entry& Entry) -> bool
		{
			const std::filesystem::path EntryPath = Entry.path();
			const std::string Name = EntryPath.filename().generic_string();
			std::error_code EntryError;
			const std::filesystem::file_status Status =
				QueryEntryStatus(Entry, EntryError);
			if (EntryError)
			{
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Entry,
					EntryPath,
					std::format("Skipped entry because its status could not be read: {}", EntryError.message()));
				return true;
			}
			else if (std::filesystem::is_symlink(Status))
				return true;
			else if (std::filesystem::is_directory(Status))
			{
				Result.Items.push_back({
					.Kind = EContentBrowserItemKind::Folder,
					.Name = Name,
					.PhysicalPath = NormalizePath(EntryPath.generic_string()),
				});
			}
			else if (std::filesystem::is_regular_file(Status)
				&& EntryPath.extension() != ".dasset")
			{
				FContentBrowserItem Item{
					.Kind = EContentBrowserItemKind::File,
					.Name = Name,
					.PhysicalPath = NormalizePath(EntryPath.generic_string()),
					.Extension = EntryPath.extension().generic_string(),
				};
				Item.FileSize = Entry.file_size(EntryError);
				if (!EntryError)
					Item.LastWriteTime = Entry.last_write_time(EntryError);
				if (EntryError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Entry,
						EntryPath,
						std::format("Skipped file because its metadata could not be read: {}", EntryError.message()));
				else
					Result.Items.push_back(std::move(Item));
			}
			return false;
		};

		const bool bCatalogDirectory = std::ranges::any_of(Catalog->Items, [&](const auto& Item) {
			const auto Directory = std::filesystem::path(Item.PhysicalPath).parent_path().generic_string();
			return Directory == PhysicalDirectory || FPaths::IsLexicalDescendantPath(Directory, PhysicalDirectory, true);
		});
		std::error_code IteratorError;
		if (!bRecursive)
		{
			std::filesystem::directory_iterator It(
				PhysicalDirectory,
				std::filesystem::directory_options::skip_permission_denied,
				IteratorError);
			const std::filesystem::directory_iterator End;
			if (IteratorError && !(bCatalogDirectory && IteratorError == std::errc::no_such_file_or_directory))
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					PhysicalDirectory,
					std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCanceled())
			{
				const std::filesystem::path EntryPath = It->path();
				AppendFilesystemEntry(*It);
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}
		else
		{
			std::filesystem::recursive_directory_iterator It(
				PhysicalDirectory,
				std::filesystem::directory_options::skip_permission_denied,
				IteratorError);
			const std::filesystem::recursive_directory_iterator End;
			if (IteratorError && !(bCatalogDirectory && IteratorError == std::errc::no_such_file_or_directory))
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					PhysicalDirectory,
					std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCanceled())
			{
				const std::filesystem::path EntryPath = It->path();
				if (AppendFilesystemEntry(*It)) It.disable_recursion_pending();
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(
						EEnumerationDiagnosticKind::Traversal,
						EntryPath,
						std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}

		if (Cancellation.IsCanceled()) return std::move(Result);
		RefreshAssetDirectoryIndex(std::move(Catalog), Cancellation);
		if (Cancellation.IsCanceled()) return std::move(Result);
		if (!bRecursive)
		{
			if (const auto It = AssetDirectoryIndex.find(PhysicalDirectory);
				It != AssetDirectoryIndex.end())
			{
				for (const auto* Asset : It->second)
				{
					if (Cancellation.IsCanceled()) return std::move(Result);
					Result.Items.push_back(*Asset);
				}
			}
		}
		else
		{
			for (const auto& [Directory, Assets] : AssetDirectoryIndex)
			{
				if (Cancellation.IsCanceled()) return std::move(Result);
				if (Directory != PhysicalDirectory
					&& !FPaths::IsLexicalDescendantPath(
						Directory, PhysicalDirectory, true))
					continue;
				for (const auto* Asset : Assets)
				{
					if (Cancellation.IsCanceled()) return std::move(Result);
					Result.Items.push_back(*Asset);
				}
			}
		}
		if (!bRecursive)
			for (const auto& [Directory, Assets] : AssetDirectoryIndex)
			{
				if (!FPaths::IsLexicalDescendantPath(Directory, PhysicalDirectory, true)) continue;
				const auto Relative = std::filesystem::path(Directory).lexically_relative(PhysicalDirectory);
				const auto Child = std::filesystem::path(PhysicalDirectory) / *Relative.begin();
				const auto ChildPath = NormalizePath(Child.generic_string());
				if (std::ranges::none_of(Result.Items, [&](const auto& Item) { return Item.PhysicalPath == ChildPath; }))
					Result.Items.push_back({.Kind = EContentBrowserItemKind::Folder,
						.Name = Child.filename().generic_string(), .PhysicalPath = ChildPath});
			}
		return std::move(Result);
	}

	auto FContentBrowserItemDiscovery::RefreshAssetDirectoryIndex(
		std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
		const Tasks::FCancellationToken& Cancellation) -> void
	{
		if (AssetCatalogSnapshot == Catalog) return;
		AssetCatalogSnapshot = std::move(Catalog);
		AssetDirectoryIndex.clear();
		for (const auto& Item : AssetCatalogSnapshot->Items)
		{
			if (Cancellation.IsCanceled())
			{
				AssetDirectoryIndex.clear();
				AssetCatalogSnapshot.reset();
				return;
			}
			const std::string Directory = NormalizePath(
				std::filesystem::path(Item.PhysicalPath)
					.parent_path()
					.generic_string());
			AssetDirectoryIndex[Directory].push_back(&Item);
		}
	}

	auto FContentBrowserItemDiscovery::CaptureDirectory(const std::string& Physical,
		std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
		const Tasks::FCancellationToken& Cancellation)
		-> FContentBrowserDirectorySnapshot
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureDirectory");
		Result = {};
		FContentBrowserDirectorySnapshot Snapshot;
		bool bCatalogDirectory = false;
		if (Catalog)
			for (const auto& Item : Catalog->Items)
			{
				const auto Directory = std::filesystem::path(Item.PhysicalPath).parent_path();
				if (NormalizePath(Directory.generic_string()) == Physical) bCatalogDirectory = true;
				if (!FPaths::IsLexicalDescendantPath(Directory.generic_string(), Physical, true)) continue;
				bCatalogDirectory = true;
				const auto Relative = Directory.lexically_relative(Physical);
				const auto Child = std::filesystem::path(Physical) / *Relative.begin();
				if (!std::ranges::contains(Snapshot.Children, Child)) Snapshot.Children.push_back(Child);
			}
		std::error_code IteratorError;
		std::filesystem::directory_iterator EntryIt(
				 Physical,
				 std::filesystem::directory_options::skip_permission_denied,
				 IteratorError);
		const std::filesystem::directory_iterator End;
		if (IteratorError && !(bCatalogDirectory && IteratorError == std::errc::no_such_file_or_directory))
			AddEnumerationDiagnostic(
				EEnumerationDiagnosticKind::Traversal,
				Physical,
				std::format("Could not enumerate directory tree node: {}", IteratorError.message()));
		while (!IteratorError && EntryIt != End && !Cancellation.IsCanceled())
		{
			const std::filesystem::directory_entry& Entry = *EntryIt;
			const std::filesystem::path EntryPath = Entry.path();
			std::error_code EntryError;
			const std::filesystem::file_status Status =
				QueryEntryStatus(Entry, EntryError);
			if (EntryError)
				AddEnumerationDiagnostic(
				EEnumerationDiagnosticKind::Entry,
				EntryPath,
				std::format("Skipped tree entry because its status could not be read: {}", EntryError.message()));
			else if (std::filesystem::is_directory(Status))
				if (!std::ranges::contains(Snapshot.Children, EntryPath)) Snapshot.Children.push_back(EntryPath);
			IteratorError.clear();
			EntryIt.increment(IteratorError);
			if (IteratorError)
				AddEnumerationDiagnostic(
				EEnumerationDiagnosticKind::Traversal,
				EntryPath,
				std::format("Directory tree traversal stopped: {}", IteratorError.message()));
		}
		std::ranges::sort(Snapshot.Children);
		Snapshot.Diagnostics = std::move(Result.Diagnostics);
		Snapshot.SuppressedDiagnosticCount = Result.SuppressedDiagnosticCount;
		return Snapshot;
	}

	auto FContentBrowserItemDiscovery::QueryEntryStatus(
		const std::filesystem::directory_entry& Entry,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return EntryStatusQuery
			? EntryStatusQuery(Entry, Error)
			: Entry.symlink_status(Error);
	}

	auto FContentBrowserItemDiscovery::AddEnumerationDiagnostic(
		EEnumerationDiagnosticKind Kind,
		const std::filesystem::path& Path,
		std::string Message) -> void
	{
		constexpr size_t MaximumDiagnostics = 8;
		if (Result.Diagnostics.size() >= MaximumDiagnostics)
		{
			++Result.SuppressedDiagnosticCount;
			return;
		}
		Result.Diagnostics.push_back({
			.Kind = Kind,
			.PhysicalPath = NormalizePath(Path.generic_string()),
			.Message = std::move(Message)});
	}

}
