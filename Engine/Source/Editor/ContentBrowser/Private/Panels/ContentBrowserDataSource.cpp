#include "Panels/ContentBrowserDataSource.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Misc/Paths.h"
#include "Profiling/Profiling.h"

namespace Durin::Editor::ContentBrowser::Private
{
	using ContentBrowserFilesystem::NormalizePath;

	auto FContentBrowserDataSource::CaptureItems(
		const std::string& PhysicalDirectory,
		bool bRecursive, std::shared_ptr<const FAssetCatalogSnapshot> Catalog,
		const FTaskCancellationToken& Cancellation)
		-> FContentBrowserItemsSnapshot
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureItems");
		Result = {};
		Result.Version = ++Version;
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

		std::error_code IteratorError;
		if (!bRecursive)
		{
			std::filesystem::directory_iterator It(
				PhysicalDirectory,
				std::filesystem::directory_options::skip_permission_denied,
				IteratorError);
			const std::filesystem::directory_iterator End;
			if (IteratorError)
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					PhysicalDirectory,
					std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCancellationRequested())
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
			if (IteratorError)
				AddEnumerationDiagnostic(
					EEnumerationDiagnosticKind::Traversal,
					PhysicalDirectory,
					std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCancellationRequested())
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

		if (Cancellation.IsCancellationRequested()) return std::move(Result);
		RefreshAssetDirectoryIndex(std::move(Catalog), Cancellation);
		if (Cancellation.IsCancellationRequested()) return std::move(Result);
		if (!bRecursive)
		{
			if (const auto It = AssetDirectoryIndex.find(PhysicalDirectory);
				It != AssetDirectoryIndex.end())
			{
				for (const FIndexedAsset& Asset : It->second)
				{
					if (Cancellation.IsCancellationRequested()) return std::move(Result);
					AppendAssetItem(*Asset.Path, *Asset.Data, Cancellation);
				}
			}
		}
		else
		{
			for (const auto& [Directory, Assets] : AssetDirectoryIndex)
			{
				if (Cancellation.IsCancellationRequested()) return std::move(Result);
				if (Directory != PhysicalDirectory
					&& !FPaths::IsLexicalDescendantPath(
						Directory, PhysicalDirectory, true))
					continue;
				for (const FIndexedAsset& Asset : Assets)
				{
					if (Cancellation.IsCancellationRequested()) return std::move(Result);
					AppendAssetItem(*Asset.Path, *Asset.Data, Cancellation);
				}
			}
		}
		return std::move(Result);
	}

	auto FContentBrowserDataSource::RefreshAssetDirectoryIndex(
		std::shared_ptr<const FAssetCatalogSnapshot> Catalog,
		const FTaskCancellationToken& Cancellation) -> void
	{
		if (AssetCatalogSnapshot == Catalog) return;
		AssetCatalogSnapshot = std::move(Catalog);
		AssetDirectoryIndex.clear();
		for (const auto& [Path, Data] : AssetCatalogSnapshot->Assets)
		{
			if (Cancellation.IsCancellationRequested())
			{
				AssetDirectoryIndex.clear();
				AssetCatalogSnapshot.reset();
				return;
			}
			const std::string Directory = NormalizePath(
				std::filesystem::path(Data.PhysicalPath)
					.parent_path()
					.generic_string());
			AssetDirectoryIndex[Directory].push_back({&Path, &Data});
		}
	}

	auto FContentBrowserDataSource::AppendAssetItem(
		const FPackagePath& Path,
		const FAssetData& Data, const FTaskCancellationToken& Cancellation) -> void
	{
		const bool bSingleAssetPackage = Data.TopLevelAssets.size() == 1;
		for (const FTopLevelAssetData& AssetData : Data.TopLevelAssets)
		{
			if (Cancellation.IsCancellationRequested()) return;
			FContentBrowserItem Item{
				AssetData.IsRedirector()
					? EContentBrowserItemKind::Redirector
					: EContentBrowserItemKind::Asset,
				bSingleAssetPackage
					? std::string(Path.GetPackageName())
					: std::string(AssetData.AssetPath.GetAssetName()),
				AssetData.AssetPath.ToString(),
				Path,
				NormalizePath(Data.PhysicalPath),
				AssetData.AssetClassName,
				".dasset"};
			Item.RedirectDestination = AssetData.RedirectDestination;
			Item.FileSize = Data.FileSize;
			Item.LastWriteTime = Data.LastWriteTime;
			Item.ThumbnailFileSize = Data.FileSize;
			Item.ThumbnailPackageFormatVersion = Data.FormatVersion;
			Item.ThumbnailLastWriteTimeTicks = Data.LastWriteTimeTicks;
			Result.Items.push_back(std::move(Item));
		}
	}

	auto FContentBrowserDataSource::CaptureDirectory(const std::string& Physical,
		const FTaskCancellationToken& Cancellation)
		-> FContentBrowserDirectorySnapshot
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureDirectory");
		Result = {};
		FContentBrowserDirectorySnapshot Snapshot;
		Snapshot.Version = ++Version;
		std::error_code IteratorError;
		std::filesystem::directory_iterator EntryIt(
				 Physical,
				 std::filesystem::directory_options::skip_permission_denied,
				 IteratorError);
		const std::filesystem::directory_iterator End;
		if (IteratorError)
			AddEnumerationDiagnostic(
				EEnumerationDiagnosticKind::Traversal,
				Physical,
				std::format("Could not enumerate directory tree node: {}", IteratorError.message()));
		while (!IteratorError && EntryIt != End && !Cancellation.IsCancellationRequested())
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
				Snapshot.Children.push_back(EntryPath);
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

	auto FContentBrowserDataSource::QueryEntryStatus(
		const std::filesystem::directory_entry& Entry,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return EntryStatusQuery
			? EntryStatusQuery(Entry, Error)
			: Entry.symlink_status(Error);
	}

	auto FContentBrowserDataSource::AddEnumerationDiagnostic(
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
