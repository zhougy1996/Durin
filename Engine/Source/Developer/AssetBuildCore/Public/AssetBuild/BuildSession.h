#pragma once

#include "AssetBuild/BuildFunction.h"

namespace Durin::Asset::Build
{
	class FBuildSession
	{
	public:
		// Stateless for local synchronous execution today; the named session
		// boundary leaves room for shared batch or remote execution context.
		ASSETBUILDCORE_API auto Build(
			const FBuildDefinition& Definition,
			const FBuildPolicy& Policy = {},
			const FBuildCancellationToken* Cancellation = nullptr) const -> FBuildOutput;
	};
}
