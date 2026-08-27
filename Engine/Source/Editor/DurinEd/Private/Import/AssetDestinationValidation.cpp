#include "Editor/Import/AssetDestinationValidation.h"

#include "Asset.h"

namespace Durin::Editor::Import
{
	namespace
	{
		auto QueryAssetDestinationOccupancy(const FAssetPath& AssetPath) -> FAssetDestinationOccupancy
		{
			const Asset::FAssetCatalogEntry Entry = Asset::FindAssetExact(AssetPath);
			return {
				.bRegistryAssetExists = Entry.Succeeded(),
				.ResidentPublicationState =
					Asset::GetResidentPackagePublicationState(AssetPath),
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
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath.generic_string() + ".dasset";
		if (!Result.bContentWritable)
		{
			Result.Message = "Choose a destination inside a content-writable mount.";
			return Result;
		}
		const FAssetDestinationOccupancy Occupancy =
			(OccupancyQuery != nullptr ? OccupancyQuery : QueryAssetDestinationOccupancy)(Result.AssetPath);
		Result.bRegistryAssetExists = Occupancy.bRegistryAssetExists;
		Result.ResidentPublicationState = Occupancy.ResidentPublicationState;
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
		else if (Result.ResidentPublicationState
			== Asset::EAssetPackagePublicationState::NewlyCreated)
			Result.Message = "A newly created unsaved package already uses this path. Save or explicitly discard it before reusing the destination.";
		else if (Result.ResidentPublicationState.has_value())
			Result.Message = "A resident package already uses this path. Close it or choose another destination.";
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
		Result.bContentWritable = Result.Mount->bContentWritable;
		Result.PhysicalPath = Resolved.PhysicalPath;
		if (!Result.bContentWritable)
			Result.Message = "Choose a directory inside a content-writable mount.";
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
} // namespace Durin::Editor::Import
