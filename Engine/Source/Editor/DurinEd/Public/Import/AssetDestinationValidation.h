#pragma once

#include "Asset/Load.h"
#include "DurinEdAPI.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::Editor
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
		bool bResidentPackageExists = false;
		bool bResidentPackageNewlyCreated = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FPackagePath RedirectDestination;
	};

	using FAssetDestinationOccupancyQuery = FAssetDestinationOccupancy (*)(const FPackagePath&);

	enum class EAssetDestinationError : uint8 { None, Path, Mount, ReadOnly, RegistryAsset, Redirector, UnsavedPackage, ResidentPackage };

	// Carries the complete side-effect-free decision for one editor asset destination.
	struct FAssetDestinationValidation
	{
		FPackagePath AssetPath;
		const FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bAssetPathValid = false;
		bool bMountedDestination = false;
		bool bContentWritable = false;
		bool bRegistryAssetExists = false;
		bool bResidentPackageExists = false;
		bool bResidentPackageNewlyCreated = false;
		EAssetDestinationOccupantKind OccupantKind =
			EAssetDestinationOccupantKind::None;
		FPackagePath RedirectDestination;
		EAssetDestinationError Error = EAssetDestinationError::None;
		std::string RequestedPath;
		EMountPathError MountCause = EMountPathError::None;
		std::optional<FObjectPathError> PathCause;

		auto AssetExists() const -> bool
		{
			return bRegistryAssetExists || bResidentPackageExists;
		}
		explicit operator bool() const
		{
			return Error == EAssetDestinationError::None;
		}
	};

	DURINED_API auto FormatAssetDestinationValidation(const FAssetDestinationValidation& Result) -> std::string;

	enum class EContentDirectoryError : uint8 { None, Path, Mount, ReadOnly };
	// Carries the side-effect-free resolution of one virtual asset directory.
	struct FContentDirectoryValidation
	{
		FPackagePath DirectoryPath;
		const FMountPoint* Mount = nullptr;
		std::filesystem::path PhysicalPath;
		bool bDirectoryPathValid = false;
		bool bMountedDestination = false;
		bool bContentWritable = false;
		EContentDirectoryError Error = EContentDirectoryError::None;
		std::string RequestedPath;
		EMountPathError MountCause = EMountPathError::None;
		std::optional<FObjectPathError> PathCause;

		explicit operator bool() const
		{
			return Error == EContentDirectoryError::None;
		}
	};

	DURINED_API auto FormatContentDirectoryValidation(const FContentDirectoryValidation& Result) -> std::string;

	// Validates a virtual asset path and queries occupancy only after Content resolution succeeds.
	DURINED_API auto InspectAssetDestination(
		std::string_view VirtualPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	// Converts a selected package filename back to its extension-free virtual asset destination.
	DURINED_API auto ClassifyAssetDestination(
		const std::filesystem::path& PhysicalPath,
		FAssetDestinationOccupancyQuery OccupancyQuery = nullptr
	) -> FAssetDestinationValidation;

	DURINED_API auto InspectContentDirectory(std::string_view VirtualPath)
		-> FContentDirectoryValidation;
	DURINED_API auto ClassifyContentDirectory(const std::filesystem::path& PhysicalPath)
		-> FContentDirectoryValidation;
} // namespace Durin::Editor
