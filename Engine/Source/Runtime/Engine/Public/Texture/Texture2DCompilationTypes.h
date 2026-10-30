#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	enum class ETexture2DCompilationPriority : uint8
	{
		Background,
		Interactive,
	};

}

#endif
