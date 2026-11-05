#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin::Editor { class FEditorDocumentManager; }
namespace Durin::Editor::DocumentUI
{
	DURINED_API auto DrawCloseConfirmation(FEditorDocumentManager& DocumentManager) -> void;
}
