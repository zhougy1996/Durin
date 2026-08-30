#pragma once

#include "LevelEditorAPI.h"
#include "Misc/Name.h"

namespace Durin
{
	class DTransactor;
}

namespace Durin
{
	class AActor;
	class DLevel;
	class DTextureCube;
}

namespace Durin::Editor::Level
{

	// Reports the actor selected by a TextureCube placement request or its rejection reason.
	struct FSkyBoxPlacementResult
	{
		AActor* Actor = nullptr;
		std::string Message;
		bool bChanged = false;

		explicit operator bool() const { return Actor != nullptr && Message.empty(); }
	};

	// Applies the viewport TextureCube placement policy through reversible level mutations.
	class FSkyBoxPlacement
	{
	public:
		LEVELEDITOR_API static auto PlaceTextureCube(
			DLevel& Level,
			DTextureCube* TextureCube,
			FName RequestedName,
			::Durin::DTransactor* Transactions,
			bool bReadOnly = false) -> FSkyBoxPlacementResult;
	};
}
