#pragma once

#include "CoreMinimal.h"

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin::TextureBuilder
{
	// Execution options do not participate in deterministic recipe identity.
	struct FBuildExecutionOptions
	{
		// Diagnostic/reference execution; production uses bounded scheduler parallelism.
		bool bParallelCompression = true;
	};

	inline constexpr uint32 ChannelCount = 4;
	inline constexpr uint32 MaxDimension = 16384;

	TEXTUREBUILD_API auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat;

	// Borrows source storage until all compression tasks drain. Family entrypoints
	// resolve Settings.bSRGB and choose PixelFormat for the entire texture.
	struct FBuildMipChainRequest
	{
		std::span<const Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
	};

	// Requires source mips validated by ValidateTexture2DSourceMips.
	TEXTUREBUILD_API auto AnalyzeTransparency(std::span<const Image::FImage> SourceMips) -> bool;

	// Requires validated source mips/settings and resolved Settings.bSRGB.
	// A single source mip generates a complete chain; supplied chains remain intact.
	// Failure returns no partial product. Errors are reported by the family entrypoint.
	TEXTUREBUILD_API auto BuildMipChain(const FBuildMipChainRequest& Request,
		const FBuildExecutionOptions* Control = nullptr) -> std::expected<FTexture2DBuildOutput, std::string>;

}
