#pragma once

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#include "Asset/Testing.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Name.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Threading/Task.h"

inline auto InitializeDObjectSystem() -> void
{
	static const bool bInitialized = []() {
		// CTest already parallelizes whole test processes. Keep each process's
		// scheduler bounded so a 14-job aggregate does not multiply the host's
		// hardware thread count across every Engine test executable.
		Durin::InitializeTaskScheduler(2);
		Durin::Testing::InitializeDObjectSystemForTests();
		return true;
	}();
	(void)bInitialized;
}

inline auto GetEngineTestModuleCallbackGate() -> Durin::FModuleOwnedCallbackGate
{
	static Durin::FModuleTestOwner Context("EngineTests.SpecializedRegistries");
	static auto Registration = Context.CreateOwnedCallbackRegistration(
		"EngineTests.SpecializedRegistries");
	return Registration.GetGate();
}

// Test cleanup follows the production target-plus-alias closure contract while
// avoiding an editor filesystem transaction in focused runtime suites.
inline auto DeleteAssetClosureForTest(
	std::initializer_list<Durin::FPackagePath> Paths)
	-> Durin::FAssetResult
{
	const std::vector<Durin::FPackagePath> DeletionPaths(Paths);
	Durin::FAssetDeletionJob Transaction;
	std::vector<Durin::FAssetDeletionBatchBlocker> Blockers;
	Durin::FAssetResult Result =
		Durin::PrepareAssetDeletionJob(
			DeletionPaths, {}, Transaction, Blockers);
	if (!Result) return Result;
	if (!Blockers.empty())
		return {
			Durin::EAssetError::InUse,
			Blockers.front().Details};
	const auto RemoveFiles = [&]() -> Durin::FAssetResult {
	for (const Durin::FAssetDeletionBatchEntry& Entry : Transaction.GetEntries())
	{
		std::error_code Error;
		if (!std::filesystem::remove(Entry.RegistryEntry.PhysicalPath, Error)
			|| Error)
			return {
				Durin::EAssetError::IoError,
				std::format(
					"Could not remove test asset {}: {}",
					Entry.RegistryEntry.PackagePath.ToString(),
					Error.message())};
		for (const std::filesystem::path& Companion : Entry.CompanionFiles)
		{
			Error.clear();
			if (!std::filesystem::remove(Companion, Error) || Error)
				return {
					Durin::EAssetError::IoError,
					std::format(
						"Could not remove test companion {}: {}",
						Companion.generic_string(), Error.message())};
		}
	}
	return {};
	};
	return Transaction.Delete({
		.Delete = RemoveFiles,
	});
}
