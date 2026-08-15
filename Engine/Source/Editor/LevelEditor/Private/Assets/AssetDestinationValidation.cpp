#include "Assets/AssetDestinationValidation.h"

#include "AssetLoad.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto QueryAssetDestinationOccupancy(const FAssetPath& AssetPath) -> FAssetDestinationOccupancy
		{
			const Asset::FAssetCatalogEntry Entry = Asset::FindAssetExact(AssetPath);
			return {
				.bRegistryAssetExists = Entry.Succeeded(),
				.bLoadedPackageExists = Asset::FindLoadedPackage(AssetPath) != nullptr,
				.bDraftPackageExists = Asset::FindDraftPackage(AssetPath) != nullptr,
				.OccupantKind = !Entry
					? EAssetDestinationOccupantKind::None
					: Entry->EntryKind == Asset::EAssetRegistryEntryKind::Redirector
						? EAssetDestinationOccupantKind::Redirector
						: EAssetDestinationOccupantKind::Asset,
				.RedirectDestination = Entry
					? Entry->RedirectDestination : FAssetPath{}
			};
		}
	} // namespace

	auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		FAssetDestinationValidation Result;
		Result.bAssetPathValid = FAssetPath::TryCreate(VirtualPath, Result.AssetPath, &Result.Message);
		if (!Result.bAssetPathValid) return Result;

		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(Result.AssetPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a destination inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.PhysicalPath = Resolved.PhysicalPath.generic_string() + ".dasset";
		const FAssetDestinationOccupancy Occupancy =
			(OccupancyQuery != nullptr ? OccupancyQuery : QueryAssetDestinationOccupancy)(Result.AssetPath);
		Result.bRegistryAssetExists = Occupancy.bRegistryAssetExists;
		Result.bLoadedPackageExists = Occupancy.bLoadedPackageExists;
		Result.bDraftPackageExists = Occupancy.bDraftPackageExists;
		Result.OccupantKind = Occupancy.OccupantKind;
		Result.RedirectDestination = Occupancy.RedirectDestination;
		if (Result.bRegistryAssetExists
			&& Result.OccupantKind == EAssetDestinationOccupantKind::Redirector)
			Result.Message = Result.RedirectDestination.IsValid()
				? std::format(
					"A redirector already occupies this path and points to {}. Run Fix Up Redirectors or choose another destination.",
					Result.RedirectDestination.ToString())
				: "A redirector already occupies this path. Repair or Fix Up the redirector before reusing the destination.";
		else if (Result.bRegistryAssetExists)
			Result.Message = "An asset already exists at this path. Choose another destination or delete the existing asset first.";
		else if (Result.bLoadedPackageExists)
			Result.Message = "A loaded package already uses this path. Close it or choose another destination.";
		else if (Result.bDraftPackageExists)
			Result.Message = "An unpublished asset draft already uses this path. Save or discard it before reusing the destination.";
		return Result;
	}

	auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery
	) -> FAssetDestinationValidation
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified)
		{
			FAssetDestinationValidation Result;
			Result.Message = Classified.Message;
			return Result;
		}

		std::filesystem::path VirtualPath(Classified.NormalizedVirtualPath);
		VirtualPath.replace_extension();
		return InspectAssetDestination(VirtualPath.generic_string(), OccupancyQuery);
	}

	auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation
	{
		FContentDirectoryValidation Result;
		Result.bDirectoryPathValid = FAssetPath::TryCreate(
			VirtualPath, Result.DirectoryPath, &Result.Message);
		if (!Result.bDirectoryPathValid) return Result;

		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(Result.DirectoryPath.GetView());
		Result.Mount = Resolved.Mount;
		if (!Resolved)
		{
			Result.Message = "Choose a directory inside a package-enabled mount.";
			return Result;
		}

		Result.bMountedDestination = true;
		Result.PhysicalPath = Resolved.PhysicalPath;
		return Result;
	}

	auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation
	{
		const PathUtilities::FAssetPathResult Classified =
			PathUtilities::ClassifyAssetPath(PhysicalPath);
		if (!Classified)
		{
			FContentDirectoryValidation Result;
			Result.Message = Classified.Message;
			return Result;
		}
		return InspectContentDirectory(Classified.NormalizedVirtualPath);
	}
} // namespace Durin::Editor::Level
