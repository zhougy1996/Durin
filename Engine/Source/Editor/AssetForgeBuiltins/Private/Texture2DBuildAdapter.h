#pragma once

#include "EncodedSourceSnapshot.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::AssetForge::Builtins
{
	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> Asset::Build::FTexture2DBuildSettings;
	auto BuildTexture2DCandidateFromSource(
		DTexture2D& Texture,
		std::span<const std::byte> EncodedBytes,
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
