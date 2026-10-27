#pragma once

#include "CoreMinimal.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "Misc/FeedbackContext.h"

namespace Durin::AssetForge::Builtins
{
	// Pure projection of an active import phase. The session owns execution and cancellation.
	ASSETFORGEBUILTINS_API auto MakeAssetImportFeedback(const FSceneImportProgress& Progress,
		std::string_view SourceFile) -> FSlowTaskSnapshot;
}
