#pragma once

#include "DurinEdAPI.h"
#include "Source/SourceReferenceIndex.h"

namespace Durin::Editor
{
	struct FMountedSourceRelocationRequest
	{
		std::string AuthoringAssetPath;
		std::string OriginalSourceVirtualPath;
		std::string DestinationSourceVirtualPath;
		std::vector<FSourceReference> AffectedAssets;
		size_t MaximumAffectedPackages = 256;
	};

	// Relocates one mounted source and updates every supplied referencing package
	// as one recoverable editor transaction.
	DURINED_API auto RelocateMountedSourceAcrossPackages(
		const FMountedSourceRelocationRequest& Request,
		std::string& OutError) -> bool;
} // namespace Durin::Editor
