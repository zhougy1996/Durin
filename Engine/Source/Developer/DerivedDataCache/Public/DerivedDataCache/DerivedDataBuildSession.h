#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"

namespace Durin::DerivedData
{
	class FBuildSession
	{
	public:
		// Stateless for local synchronous execution today; the named session
		// boundary leaves room for shared batch or remote execution context.
		DERIVEDDATACACHE_API auto Build(
			const FBuildDefinition& Definition,
			const FBuildPolicy& Policy = {},
			const FBuildCancellationToken* Cancellation = nullptr) const -> FBuildOutput;
	};
}
