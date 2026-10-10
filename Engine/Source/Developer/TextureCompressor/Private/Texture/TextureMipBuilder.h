#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin::TextureMipBuilder
{
	// Execution options do not participate in deterministic recipe identity.
	struct FBuildExecutionOptions
	{
		// Diagnostic/reference execution; production uses bounded scheduler parallelism.
		bool bParallelCompression = true;
	};

	inline constexpr uint32 ChannelCount = 4;
	inline constexpr uint32 MaxDimension = 16384;

	TEXTURECOMPRESSOR_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Borrows source storage until all compression tasks drain. Family entrypoints
	// resolve Settings.bSRGB and choose PixelFormat for the entire texture.
	struct FBuildTextureMipsRequest
	{
		std::span<const Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
	};

	// Requires source mips validated by ValidateTexture2DSourceMips.
	TEXTURECOMPRESSOR_API auto AnalyzeTransparency(std::span<const Image::FImage> SourceMips) -> bool;

	// Requires validated source mips/settings and resolved Settings.bSRGB.
	// A single source mip generates a complete chain; supplied chains remain intact.
	// Failure returns no partial product. Errors are reported by the family entrypoint.
	TEXTURECOMPRESSOR_API auto BuildTextureMips(const FBuildTextureMipsRequest& Request,
		const FBuildExecutionOptions* ExecutionOptions = nullptr) -> std::expected<FTexture2DBuildOutput, std::string>;

}
