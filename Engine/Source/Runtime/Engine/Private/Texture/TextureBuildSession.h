#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin
{
	namespace TexturePrivate
	{
		auto RegisterBuildFunctions() -> void;
		auto BuildAsync(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::IRequestOwner& Owner,
			DerivedData::FBuildCompletionCallback Completion,
			DerivedData::FBuildRequestOptions Options = {}) -> bool;
		auto Build(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options = {})
			-> std::optional<DerivedData::FBuildCompleteParams>;
	}
}
#endif
