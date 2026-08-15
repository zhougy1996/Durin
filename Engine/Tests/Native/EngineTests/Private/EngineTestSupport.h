#pragma once

#include "AssetMutation.h"
#include "AssetTestSupport.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EngineAssetServices.h"
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
		Durin::InitializeEngineAssetServices();
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
	std::initializer_list<Durin::FAssetPath> Paths)
	-> Durin::Asset::FAssetResult
{
	const std::vector<Durin::FAssetPath> DeletionPaths(Paths);
	Durin::Asset::FAssetDeletionTransaction Transaction;
	std::vector<Durin::Asset::FAssetDeletionBatchBlocker> Blockers;
	Durin::Asset::FAssetResult Result =
		Durin::Asset::PrepareAssetDeletionTransaction(
			DeletionPaths, {}, Transaction, Blockers);
	if (!Result) return Result;
	if (!Blockers.empty())
		return {
			Durin::Asset::EAssetError::InUse,
			Blockers.front().Details};
	const auto RemoveFiles = [&]() -> Durin::Asset::FAssetResult {
	for (const Durin::Asset::FAssetDeletionBatchEntry& Entry : Transaction.GetEntries())
	{
		std::error_code Error;
		if (!std::filesystem::remove(Entry.RegistryEntry.PhysicalPath, Error)
			|| Error)
			return {
				Durin::Asset::EAssetError::IoError,
				std::format(
					"Could not remove test asset {}: {}",
					Entry.RegistryEntry.PackagePath.ToString(),
					Error.message())};
		for (const std::filesystem::path& Companion : Entry.CompanionFiles)
		{
			Error.clear();
			if (!std::filesystem::remove(Companion, Error) || Error)
				return {
					Durin::Asset::EAssetError::IoError,
					std::format(
						"Could not remove test companion {}: {}",
						Companion.generic_string(), Error.message())};
		}
	}
	return {};
	};
	return Transaction.Commit({
		.Stage = RemoveFiles,
		.Restore = [] { return Durin::Asset::FAssetResult{
			Durin::Asset::EAssetError::IoError,
			"Irreversible test cleanup cannot be restored."}; },
	});
}
