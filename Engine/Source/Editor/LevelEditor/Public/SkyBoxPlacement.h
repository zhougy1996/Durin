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

	enum class ESkyBoxPlacementError : uint8 { None, ReadOnly, TextureUnavailable, MultipleSkyBoxes, SkyBoxUnavailable, Transaction, Replay };
	// Command status and presentation text hide transaction implementation details.
	struct FSkyBoxPlacementResult
	{
		AActor* Actor = nullptr;
		ESkyBoxPlacementError Error = ESkyBoxPlacementError::None;
		std::string Message;
		bool bChanged = false;
		explicit operator bool() const { return Error == ESkyBoxPlacementError::None; }
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
