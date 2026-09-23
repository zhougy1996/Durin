#pragma once

#include "EngineAPI.h"
#include "RHIDefinitions.h"
#include "Texture/Texture2DData.h"

#include "TextureCubeData.gen.h"

namespace Durin
{
	class FArchive;

	// Selects display-ready LDR or preserved linear panorama radiance.
	DENUM(DisplayName = "Texture Cube Output")
	enum class ETextureCubeOutput : uint8
	{
		LDR = 0,
		HDR = 1,
	};

	DENUM(DisplayName = "Texture Cube Source Layout")
	enum class ETextureCubeSourceLayout : uint8
	{
		SixFaces,
		EquirectangularPanorama DMETA(DisplayName = "Equirectangular Panorama"),
	};

	// Immutable shared RGBA8 images used by decoding, projection, and build recipes.
	// Faces use Unknown gamma; the cube build settings supply color interpretation.
	struct FTextureCubeDecodedFaces
	{
		std::array<Image::FImage, TextureCubeFaceCount> Faces;
		std::array<uint8, TextureCubeFaceCount> SourceChannelCounts{};
		uint8 TransparencyMask = 0;

		ENGINE_API auto IsValid() const -> bool;
	};

	struct FTextureCubePlatformData
	{
		std::array<FTexturePlatformData, TextureCubeFaceCount> Faces;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		ENGINE_API auto IsValid() const -> bool;
		// Loads canonical six-slice TXPL in place; discard failures and check the owning byte boundary.
		ENGINE_API auto Serialize(
			FArchive& Ar) -> void;
	};

}
