#pragma once

#include "Asset/AssetDefinitions.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"

namespace Durin::AssetPrivate
{
	// One load invocation's dependency ownership. Supply all callbacks or none.
	// Callbacks and returned objects must outlive graph application. Rollback owns
	// only dependencies admitted by this invocation, after its graph is discarded.
	// The optional guard rejects synchronous live loads on the calling thread,
	// including constructors, PostLoad, and policy callbacks; it does not pin code.
	struct FAssetPackageDependencyLoadPolicy
	{
		std::function<FAssetResult(const FPackagePath&, DPackage*&)> ResolvePackage;
		std::function<FAssetResult(const FObjectPath&, DObject*&)> ResolveObject;
		std::function<void()> Rollback;
		bool bRejectImplicitLiveLoads = false;
	};
}
