#pragma once

#include "CoreMinimal.h"

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

#if DURIN_WITH_EDITORONLY_DATA
	// Shared immutable RGBA8 pixels used by decoding, projection, and offline builds.
	// Faces use Unknown gamma; the cube build settings supply color interpretation.
	struct FTextureCubeFaceImages
	{
		std::array<Image::FImage, TextureCubeFaceCount> Faces;
		uint8 SourceChannelCount = 0;
		uint8 TransparencyMask = 0;

		ENGINE_API auto IsValid() const -> bool;
	};
#endif

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
