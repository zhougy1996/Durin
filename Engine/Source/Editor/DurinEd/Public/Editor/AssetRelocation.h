#pragma once

#include "Asset/MutationExtensions.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	class FTransactionManager;

	// Commits AssetCore relocations and retains their reversible transaction in
	// the process-wide editor history.
	DURINED_API auto ExecuteAssetRelocations(
		FTransactionManager& Transactions,
		std::span<const Asset::FAssetRelocationMapping> Mappings)
		-> Asset::FAssetResult;
} // namespace Durin::Editor
