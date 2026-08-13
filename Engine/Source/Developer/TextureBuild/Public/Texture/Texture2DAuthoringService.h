#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DAuthoringCoordinator.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::AssetBuild
{
	using FTexture2DAuthoringCompletion = std::function<void(bool, std::string)>;

	struct FTexture2DAuthoringRequest
	{
		FTextureSourceData SourceData;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		FSourcePath SourcePath;
		FTexture2DBuildSettings Settings;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		uint64 SourceFileSize = 0;
		int64 SourceLastWriteTime = 0;
		ETexture2DBuildPriority Priority = ETexture2DBuildPriority::Background;
		bool bPersistDerivedData = true;
		bool bMarkPackageDirty = true;
		bool bReportLoadMutation = false;
	};

	TEXTUREBUILD_API auto SubmitTexture2DBuild(
		DTexture2D& Texture,
		FTexture2DAuthoringRequest Request,
		std::string& OutError,
		FTexture2DAuthoringCompletion Completion = {}) -> bool;
	TEXTUREBUILD_API auto GetTexture2DBuildDiagnostic(const DTexture2D& Texture)
		-> FTexture2DBuildDiagnostic;
	TEXTUREBUILD_API auto HasPendingTexture2DBuild(const DTexture2D& Texture) -> bool;
	TEXTUREBUILD_API auto CancelTexture2DBuild(DTexture2D& Texture) -> bool;
	TEXTUREBUILD_API auto WaitForTexture2DBuild(
		DTexture2D& Texture,
		double TimeoutSeconds = 300.0) -> bool;
}
