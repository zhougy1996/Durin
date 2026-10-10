#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "Texture/VolumeTextureBuild.h"
namespace Durin
{
	class ITextureCompressorModule;
	struct FVolumeTextureBuildKeyInput;
	namespace TexturePrivate
	{
		ENGINE_API auto MakeVolumeTextureBuildFunction(ITextureCompressorModule& Module) -> std::shared_ptr<const DerivedData::IBuildFunction>;
		ENGINE_API auto MakeVolumeTextureInputResolver(const FTextureSource& Source) -> std::shared_ptr<const DerivedData::IBuildInputResolver>;
		auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildKeyInput& Input)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
		ENGINE_API auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildRequest& Request)
			-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	}
}
#endif
