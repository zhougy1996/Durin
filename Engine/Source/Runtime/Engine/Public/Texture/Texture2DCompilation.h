#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/Texture2DCompilationTypes.h"
#include "Texture/TextureBuildOperation.h"
#include "Texture/Texture2DBuild.h"

namespace Durin
{
	struct FTexture2DCompilationRequest
	{
		// Detached worker input and derived-data policy.
		FTexture2DBuildRequest Build;
		// Main-thread result application and source provenance.
		FTexture2DResultApplicationContext ResultApplication;
		ETexture2DCompilationPriority Priority = ETexture2DCompilationPriority::Background;
	};
}

#endif
