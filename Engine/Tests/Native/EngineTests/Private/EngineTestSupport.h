#pragma once

#include "NativeAssetTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/Testing.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Name.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "World/WorldServiceTestSupport.h"
#include "Threading/Task.h"

inline auto InitializeDObjectSystem() -> void
{
	static const bool bInitialized = []() {
		// CTest already parallelizes whole test processes. Keep each process's
		// scheduler bounded so a 14-job aggregate does not multiply the host's
		// hardware thread count across every Engine test executable.
		Durin::InitializeTaskScheduler(2);
		Durin::Testing::InitializeDObjectSystemForTests();
		RegisterWorldServicesForTests();
		return true;
	}();
	(void)bInitialized;
}

// Runtime fixture cleanup uses only the package removal primitives.
inline auto DeleteAssetClosureForTest(std::initializer_list<Durin::FPackagePath> Paths)
	-> Durin::FAssetResult
{
	return Durin::Testing::RemoveAssetPackagesForTests(std::span{Paths.begin(), Paths.size()});
}
