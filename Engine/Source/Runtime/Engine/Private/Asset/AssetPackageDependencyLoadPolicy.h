#pragma once

#include "Asset/AssetReadResult.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"

namespace Durin::AssetPrivate
{
	// One load invocation's dependency bindings. Supply both resolvers or neither.
	// Callbacks and returned objects must outlive graph application. Rollback owns
	// only explicit private-policy dependencies after its graph is discarded.
	// Deferred ordinary loading omits Rollback; the load service owns incomplete groups.
	// The optional guard rejects synchronous live loads on the calling thread,
	// including constructors, PostLoad, and policy callbacks; it does not pin code.
	struct FAssetPackageDependencyLoadPolicy
	{
		std::function<FAssetReadResult(const FPackagePath&, DPackage*&)> ResolvePackage;
		std::function<FAssetReadResult(const FObjectPath&, DObject*&)> ResolveObject;
		std::function<void()> Rollback;
		bool bRejectImplicitLiveLoads = false;
	};
}
