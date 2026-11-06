#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin { class FEditorDocumentManager; }
namespace Durin::DocumentUI
{
	// Selects the oldest outstanding close across controllers; unrelated opens cannot retarget a live prompt.
	DURINED_API auto FindPendingClose(std::span<FEditorDocumentManager* const> Controllers) -> FEditorDocumentManager*;
	DURINED_API auto DrawCloseConfirmation(FEditorDocumentManager& DocumentManager) -> void;
}
