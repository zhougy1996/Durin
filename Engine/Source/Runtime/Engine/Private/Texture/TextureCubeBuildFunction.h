#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "TextureDerivedDataKey.h"
#include "Texture/TextureCubeBuildTypes.h"
namespace Durin
{
	class ITextureBuildModule;
	namespace TexturePrivate
	{
		ENGINE_API auto MakeTextureCubeBuildFunction(ITextureBuildModule& Module) -> std::shared_ptr<const DerivedData::IBuildFunction>;
		ENGINE_API auto MakeTextureCubeInputResolver(const FTextureSource& Source, const FTextureCubeCanonicalBuildInput* Prepared)
			-> std::shared_ptr<const DerivedData::IBuildInputResolver>;
		ENGINE_API auto MakeTextureCubeSessionDefinition(const FTextureCubeBuildKeyInput& Input)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	}
}
#endif
