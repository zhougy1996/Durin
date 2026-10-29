#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Identifies the externally visible phase of one object-level Texture2D compilation.
	enum class ETexture2DCompilationPhase : uint8
	{
		None,
		Queued,
		Building,
		AwaitingApply,
		Ready,
		Failed,
		Canceled,
	};

	// Small object-level snapshot for editor progress and failure presentation.
	struct FTexture2DBuildStatus
	{
		ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::None;
		bool bPending = false;
		std::string FailureMessage;
	};


	enum class ETexture2DCompilationPriority : uint8
	{
		Background,
		Interactive,
	};

}

#endif
