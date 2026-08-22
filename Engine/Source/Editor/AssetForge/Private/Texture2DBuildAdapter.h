#pragma once

#include "EncodedSourceSnapshot.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::Asset::Forge
{
	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> Asset::Build::FTexture2DBuildSettings;
	auto BuildTexture2DCandidateFromSource(
		DTexture2D& Texture,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError,
		int64 SourceLastWriteTime = 0) -> bool;
	auto BuildTexture2DCandidateFromSnapshot(
		DTexture2D& Texture,
		const FEncodedSourceSnapshot& Source,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool;
}
