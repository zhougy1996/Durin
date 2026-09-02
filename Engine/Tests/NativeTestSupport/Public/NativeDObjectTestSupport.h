#pragma once

#include "CoreGlobals.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
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

namespace Durin
{
	struct FTestAssetCreateResult
	{
		std::string Message;

		explicit operator bool() const { return Message.empty(); }
	};

	// Constructs a package-leaf top-level object for fixtures without exposing
	// an Engine asset-creation operation to production callers.
	template<typename T>
	auto CreatePackageLeafAssetForTesting(
		const FPackagePath& PackagePath,
		T*& OutAsset) -> FTestAssetCreateResult
	{
		OutAsset = nullptr;
		if (!PackagePath.IsValid()) return {"The fixture package path is invalid."};
		DPackage* Package = CreatePackage(PackagePath);
		if (!Package) return {"The fixture package could not be created."};
		FStaticConstructObjectParameters Parameters{
			T::StaticClass(), Package, FName(PackagePath.GetPackageName()),
			sizeof(T), EObjectFlags::Public};
		DObject* Object = StaticConstructObject(Parameters);
		DObjectForceRegistration(Object);
		OutAsset = Cast<T>(Object);
		if (!OutAsset || Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
		{
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return {"The fixture object could not be registered as a top-level asset."};
		}
		Package->MarkDirty();
		Package->MarkAsNewlyCreated();
		return {};
	}
}
