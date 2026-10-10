#pragma once

#include "CoreMinimal.h"

#include "TextureBuildAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin::TextureBuilder
{
	// Execution options do not participate in deterministic recipe identity.
	struct FBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		// Diagnostic observation during synchronous execution; never needed by production.
		FTexture2DBuildTimings* DiagnosticMetrics = nullptr;
		// Diagnostic/reference execution; production uses bounded scheduler parallelism.
		bool bParallelCompression = true;
	};

	inline constexpr uint32 ChannelCount = 4;
	inline constexpr uint32 MaxDimension = 16384;
	inline constexpr uint32 CancellationBlockInterval = 64;
	inline constexpr uint32 CancellationScanlineInterval = 8;

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
	TEXTUREBUILD_API auto AnalyzeTransparency(std::span<const Image::FImage> SourceMips,
		const FBuildExecutionControl* Control = nullptr) -> std::expected<bool, FTexture2DBuildError>;

	// Requires validated source mips/settings and resolved Settings.bSRGB.
	// A single source mip generates a complete chain; supplied chains remain intact.
	// Failure returns no partial product. Diagnostic metrics may describe completed work.
	TEXTUREBUILD_API auto BuildMipChain(const FBuildMipChainRequest& Request,
		const FBuildExecutionControl* Control = nullptr) -> std::expected<FTexture2DBuildOutput, FTexture2DBuildError>;

}
