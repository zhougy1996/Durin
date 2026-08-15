#pragma once

#include "AssetBuild/BuildFunction.h"

namespace Durin::Asset::Build
{
	class FBuildSession
	{
	public:
		ASSETBUILDCORE_API auto Build(
			const FBuildDefinition& Definition,
			const FBuildPolicy& Policy = {},
			const FBuildCancellationToken* Cancellation = nullptr) const -> FBuildOutput;
	};
}
