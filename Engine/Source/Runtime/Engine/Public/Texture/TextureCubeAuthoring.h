#pragma once

#include "EngineAPI.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	using FTextureCubePanoramaBuildHandler = std::function<bool(
		DTextureCube&, std::span<const uint8>, std::string_view, const FSourcePath&,
		const FTextureCubePanoramaImportSettings&, std::string&)>;
	using FTextureCubeFacesBuildHandler = std::function<bool(
		DTextureCube&,
		const std::array<std::span<const uint8>, TextureCubeFaceCount>&,
		const std::array<FSourcePath, TextureCubeFaceCount>&,
		const FTextureCubeImportSettings&, std::string&)>;
	using FTextureCubeRebuildHandler = std::function<bool(DTextureCube&, std::string&)>;
	using FTextureCubeFacesValidationHandler = std::function<FTextureCubeImportValidation(
		const std::array<std::string, TextureCubeFaceCount>&,
		const FTextureCubeImportSettings&)>;
	using FTextureCubePanoramaValidationHandler = std::function<FTextureCubeImportValidation(
		std::string_view, const FTextureCubePanoramaImportSettings&)>;

	struct FTextureCubeAuthoringHandlers
	{
		FTextureCubePanoramaBuildHandler BuildPanorama;
		FTextureCubeFacesBuildHandler BuildFaces;
		FTextureCubeRebuildHandler Rebuild;
		FTextureCubeFacesValidationHandler ValidateFaces;
		FTextureCubePanoramaValidationHandler ValidatePanorama;
	};

	ENGINE_API auto RegisterTextureCubeAuthoringHandlers(
		FTextureCubeAuthoringHandlers Handlers) -> bool;
	ENGINE_API auto UnregisterTextureCubeAuthoringHandlers() -> void;
	ENGINE_API auto GetTextureCubeAuthoringHandlers() -> FTextureCubeAuthoringHandlers;
}
