#pragma once

#include "CoreGlobals.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Name.h"

namespace Durin::Testing
{
	inline auto MakeTopLevelAssetObjectPathForTests(
		const FPackagePath& PackagePath, std::string_view AssetName) -> FObjectPath
	{
		FTopLevelAssetPath AssetPath;
		require(FTopLevelAssetPath::TryCreate(PackagePath, AssetName, AssetPath));
		FObjectPath ObjectPath;
		require(FObjectPath::TryCreate(
			AssetPath, std::span<const std::string>{}, ObjectPath));
		return ObjectPath;
	}

	inline auto MakePackageLeafTopLevelAssetPathForTests(
		const FPackagePath& PackagePath) -> FTopLevelAssetPath
	{
		FTopLevelAssetPath AssetPath;
		require(FTopLevelAssetPath::TryCreate(
			PackagePath, PackagePath.GetPackageName(), AssetPath));
		return AssetPath;
	}

	// Builds the exact top-level object identity for fixtures that intentionally
	// preserve the former package-leaf asset naming convention.
	inline auto MakePackageLeafAssetObjectPathForTests(
		const FPackagePath& PackagePath) -> FObjectPath
	{
		return MakeTopLevelAssetObjectPathForTests(
			PackagePath, PackagePath.GetPackageName());
	}

	inline auto InitializeDObjectSystemForTests() -> void
	{
		// GoogleTest death tests fork an already-initialized process on POSIX.
		// Refresh the native thread identity in the child before consulting the
		// inherited one-time DObject initialization state.
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		static const bool bInitialized = [] {
			if (!IsFNameInitialized()) FNameInit();
			DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}
}
