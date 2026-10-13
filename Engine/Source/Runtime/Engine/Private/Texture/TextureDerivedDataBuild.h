#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuildTypes.h"
#include "Texture/VolumeTextureBuild.h"
#include "TextureDerivedDataKey.h"

namespace Durin::TexturePrivate
{
	ENGINE_API auto MakeTexture2DBuildFunction()
		-> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeTexture2DInputResolver(const FTextureSource& Source)
		-> std::shared_ptr<const DerivedData::IBuildInputResolver>;
	auto MakeTexture2DSessionDefinition(const FTexture2DBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeTexture2DSessionDefinition(const FTexture2DBuildRequest& Request)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;

	ENGINE_API auto MakeTextureCubeBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeTextureCubeInputResolver(const FTextureSource& Source, const FTextureCubeCanonicalBuildInput* Prepared)
		-> std::shared_ptr<const DerivedData::IBuildInputResolver>;
	ENGINE_API auto MakeTextureCubeSessionDefinition(const FTextureCubeBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;

	ENGINE_API auto MakeVolumeTextureBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeVolumeTextureInputResolver(const FTextureSource& Source) -> std::shared_ptr<const DerivedData::IBuildInputResolver>;
	auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto MakeVolumeTextureSessionDefinition(const FVolumeTextureBuildRequest& Request)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;

	auto RegisterBuildFunctions() -> void;
	auto ShutdownBuildSessions() -> void;
	auto BuildAsync(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::IRequestOwner& Owner,
		DerivedData::FBuildCompletionCallback Completion,
		DerivedData::FBuildRequestOptions Options = {}) -> std::expected<void, DerivedData::FBuildAdmissionError>;
	ENGINE_API auto Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOptions Options = {})
		-> DerivedData::FBuildCompleteParams;
}
#endif
