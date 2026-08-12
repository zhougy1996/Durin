#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/Texture2D.h"

namespace Durin::AssetBuild
{
	// Authoring-only entry point for producing a detached Texture2D candidate.
	// Providers own encoded source capture; EngineAssetBuild owns interpretation
	// and platform-data production before the Engine object is published.
	ENGINEASSETBUILD_API auto BuildTexture2DFromEncodedBytes(
		DTexture2D& Texture,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool;

	ENGINEASSETBUILD_API auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTexture2DImportResult;
}
