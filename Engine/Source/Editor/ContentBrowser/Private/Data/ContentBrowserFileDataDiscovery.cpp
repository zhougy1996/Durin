#include "Data/ContentBrowserFileDataDiscovery.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"

namespace Durin::ContentBrowser::Private
{
	using ContentBrowserFilesystem::NormalizePath;

	auto FContentBrowserFileDataDiscovery::CaptureItems(const std::string& PhysicalDirectory, bool bRecursive,
														const Tasks::FCancellationToken& Cancellation) -> FContentBrowserDataCapture
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureItems");
		Result = {};
		if (PhysicalDirectory.empty())
			return std::move(Result);
		auto AppendFilesystemEntry = [&](const std::filesystem::directory_entry& Entry) -> bool
		{
			const std::filesystem::path EntryPath = Entry.path();
			const std::string Name = EntryPath.filename().generic_string();
			std::error_code EntryError;
			const std::filesystem::file_status Status = QueryEntryStatus(Entry, EntryError);
			if (EntryError)
			{
				AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Entry, EntryPath,
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
			else if (std::filesystem::is_regular_file(Status) && EntryPath.extension() != ".dasset")
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
					AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Entry, EntryPath,
											 std::format("Skipped file because its metadata could not be read: {}", EntryError.message()));
				else
					Result.Items.push_back(std::move(Item));
			}
			return false;
		};

		std::error_code IteratorError;
		if (!bRecursive)
		{
			std::filesystem::directory_iterator It(PhysicalDirectory, std::filesystem::directory_options::skip_permission_denied,
												   IteratorError);
			const std::filesystem::directory_iterator End;
			if (IteratorError && !(IteratorError == std::errc::no_such_file_or_directory))
				AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Traversal, PhysicalDirectory,
										 std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCanceled())
			{
				const std::filesystem::path EntryPath = It->path();
				AppendFilesystemEntry(*It);
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Traversal, EntryPath,
											 std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}
		else
		{
			std::filesystem::recursive_directory_iterator It(PhysicalDirectory, std::filesystem::directory_options::skip_permission_denied,
															 IteratorError);
			const std::filesystem::recursive_directory_iterator End;
			if (IteratorError && !(IteratorError == std::errc::no_such_file_or_directory))
				AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Traversal, PhysicalDirectory,
										 std::format("Could not enumerate directory: {}", IteratorError.message()));
			while (!IteratorError && It != End && !Cancellation.IsCanceled())
			{
				const std::filesystem::path EntryPath = It->path();
				if (AppendFilesystemEntry(*It))
					It.disable_recursion_pending();
				IteratorError.clear();
				It.increment(IteratorError);
				if (IteratorError)
					AddEnumerationDiagnostic(EEnumerationDiagnosticKind::Traversal, EntryPath,
											 std::format("Directory traversal stopped: {}", IteratorError.message()));
			}
		}

		return std::move(Result);
	}

	auto FContentBrowserFileDataDiscovery::QueryEntryStatus(const std::filesystem::directory_entry& Entry, std::error_code& Error) const
		-> std::filesystem::file_status
	{
		return EntryStatusQuery ? EntryStatusQuery(Entry, Error) : Entry.symlink_status(Error);
	}

	auto FContentBrowserFileDataDiscovery::AddEnumerationDiagnostic(EEnumerationDiagnosticKind Kind, const std::filesystem::path& Path,
																	std::string Message) -> void
	{
		constexpr size_t MaximumDiagnostics = 8;
		if (Result.Diagnostics.size() >= MaximumDiagnostics)
		{
			++Result.SuppressedDiagnosticCount;
			return;
		}
		Result.Diagnostics.push_back({.Kind = Kind, .PhysicalPath = NormalizePath(Path.generic_string()), .Message = std::move(Message)});
	}

} // namespace Durin::ContentBrowser::Private
