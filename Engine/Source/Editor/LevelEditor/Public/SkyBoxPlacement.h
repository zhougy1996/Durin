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

namespace Durin::Editor
{
	struct FTransactorResult;
	struct FTransactionCustomError;
}

namespace Durin::Editor::Level
{

	enum class ESkyBoxPlacementError : uint8 { None, ReadOnly, TextureUnavailable, MultipleSkyBoxes, SkyBoxUnavailable, Transaction, Replay };
	struct FSkyBoxPlacementError
	{
		ESkyBoxPlacementError Code = ESkyBoxPlacementError::None;
		std::string LevelPath;
		std::string RequestedName;
		size_t CandidateCount = 0;
		std::shared_ptr<const FTransactorResult> TransactionCause;
		std::shared_ptr<const FTransactionCustomError> ReplayCause;
	};
	// Reports placement outcome separately from typed failure evidence.
	struct FSkyBoxPlacementResult
	{
		AActor* Actor = nullptr;
		FSkyBoxPlacementError Error;
		bool bChanged = false;
		explicit operator bool() const { return Error.Code == ESkyBoxPlacementError::None; }
	};
	LEVELEDITOR_API auto FormatSkyBoxPlacementError(const FSkyBoxPlacementError& Error) -> std::string;

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
