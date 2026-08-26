#pragma once

#include "DurinEdAPI.h"
#include "Source/SourceReferenceIndex.h"

namespace Durin
{
	class DObject;
}

namespace Durin::Editor
{
	using FMountedSourceRelocationHandler = std::function<std::optional<bool>(
		DObject&, std::string_view, std::string_view, std::string&)>;
	using FMountedSourceRelocationHandlerHandle = uint64;

	DURINED_API auto RegisterMountedSourceRelocationHandler(
		FMountedSourceRelocationHandler Handler)
		-> FMountedSourceRelocationHandlerHandle;
	DURINED_API auto UnregisterMountedSourceRelocationHandler(
		FMountedSourceRelocationHandlerHandle Handle) -> void;

	struct FMountedSourceRelocationRequest
	{
		std::string ReferencingAssetPath;
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
