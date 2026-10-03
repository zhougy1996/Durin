#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "Texture/Texture2DBuild.h"

namespace Durin
{
	class ITextureBuildModule;
	struct FTexture2DBuildKeyInput;
	namespace TexturePrivate
	{
		ENGINE_API auto MakeTexture2DBuildFunction(ITextureBuildModule& Module)
			-> std::shared_ptr<const DerivedData::IBuildFunction>;
		ENGINE_API auto MakeTexture2DInputResolver(const FTextureSource& Source)
			-> std::shared_ptr<const DerivedData::IBuildInputResolver>;
		auto MakeTexture2DSessionDefinition(const FTexture2DBuildKeyInput& Input)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
		ENGINE_API auto MakeTexture2DSessionDefinition(const FTexture2DBuildRequest& Request)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	}
}
#endif
