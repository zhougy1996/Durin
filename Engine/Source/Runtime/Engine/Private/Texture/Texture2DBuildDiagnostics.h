#pragma once

#include "Texture/Texture2DCompilationTypes.h"

namespace Durin::TexturePrivate
{
	inline auto MakeCompilationBuildFailure(const FTexture2DBuildError& Error) -> FTexture2DCompilationError
	{
		if (Error.Code == ETexture2DBuildError::Cancelled)
			return {.Code = ETexture2DCompilationError::Cancelled};
		FTexture2DCompilationError Result{.Code = ETexture2DCompilationError::BuildFailed};
		if (Error.InputCause || Error.Code == ETexture2DBuildError::InvalidInput
			|| Error.Code == ETexture2DBuildError::InvalidCompressionQuality
			|| Error.Code == ETexture2DBuildError::InvalidUsage
			|| Error.Code == ETexture2DBuildError::InvalidAlphaMipMode
			|| Error.Code == ETexture2DBuildError::InvalidAlphaCoverageThreshold)
			Result.InputReason = FormatTexture2DBuildError(Error);
		return Result;
	}
}
