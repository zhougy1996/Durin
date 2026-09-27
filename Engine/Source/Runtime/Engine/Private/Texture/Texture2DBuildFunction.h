#pragma once
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "Texture/Texture2DBuild.h"

namespace Durin
{
	class ITextureBuildModule;
	namespace TexturePrivate
	{
		// Provider lifetime is owned by the Engine build service and drained before unload.
		ENGINE_API auto MakeTexture2DBuildFunction(ITextureBuildModule& Module)
			-> std::shared_ptr<const DerivedData::IBuildFunction>;
		ENGINE_API auto MakeTexture2DInputResolver(const FTextureSource& Source)
			-> std::shared_ptr<const DerivedData::IBuildInputResolver>;
		ENGINE_API auto MakeTexture2DSessionDefinition(const FTexture2DBuildRequest& Request)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	}
}
#endif
