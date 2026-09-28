#pragma once
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin::AssetBuildPrivate
{
	// Private synchronous bridge for already-admitted owner workers. All asset
	// families share the persistent service session and supply request-owned input.
	ENGINE_API auto Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOptions Options = {}) -> DerivedData::FBuildCompleteParams;
}
#endif
