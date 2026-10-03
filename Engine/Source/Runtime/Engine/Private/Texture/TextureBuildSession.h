#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin
{
	class ITextureBuildModule;
	namespace TexturePrivate
	{
		auto RegisterBuildFunctions(ITextureBuildModule& Module) -> void;
		auto Build(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options = {})
			-> std::optional<DerivedData::FBuildCompleteParams>;
	}
}
#endif
