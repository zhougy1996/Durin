#pragma once

#include "DObject/AssetPath.h"
#include "Misc/Paths.h"

namespace Durin::Editor::Level
{
	enum class EAssetDestinationOccupantKind : uint8
	{
		None,
		Asset,
		Redirector
	};

	// Separates asset occupancy lookup from path validation for deterministic callers and tests.
	struct FAssetDestinationOccupancy
	{
		bool bRegistryAssetExists = false;
		bool bLoadedPackageExists = false;
		bool bDraftPackageExists = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FAssetPath RedirectDestination;
	};

	using FAssetDestinationOccupancyQuery = FAssetDestinationOccupancy (*)(const FAssetPath&);

	// Carries the complete side-effect-free decision for one editor asset destination.
	struct FAssetDestinationValidation
	{
		FAssetPath AssetPath;
		const PathUtilities::FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bAssetPathValid = false;
		bool bMountedDestination = false;
		bool bRegistryAssetExists = false;
		bool bLoadedPackageExists = false;
		bool bDraftPackageExists = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FAssetPath RedirectDestination;
		std::string Message;

		auto AssetExists() const -> bool
		{
			return bRegistryAssetExists || bLoadedPackageExists || bDraftPackageExists;
		}
		explicit operator bool() const
		{
			return bAssetPathValid && bMountedDestination && !AssetExists() && Message.empty();
		}
	};

	// Carries the side-effect-free resolution of one virtual asset directory.
	struct FContentDirectoryValidation
	{
		FAssetPath DirectoryPath;
		const PathUtilities::FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bDirectoryPathValid = false;
		bool bMountedDestination = false;
		std::string Message;

		explicit operator bool() const
		{
			return bDirectoryPathValid && bMountedDestination && Message.empty();
		}
	};

	// Validates a virtual asset path and queries occupancy only after Content resolution succeeds.
	auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	// Converts a selected package filename back to its extension-free virtual asset destination.
	auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation;
	auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation;
} // namespace Durin::Editor::Level
