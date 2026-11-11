#pragma once

#include "CoreMinimal.h"

#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
namespace Durin::LevelEditor::Testing
{
	enum class EStaticMeshLevelMutationFailurePoint : uint8
	{
		None,
		AfterTemporaryRename,
		AfterRemove,
		AfterCreate,
		AfterFinalRename,
		AfterUpdate,
	};

	auto SetStaticMeshLevelMutationFailurePoint(
		EStaticMeshLevelMutationFailurePoint Point) -> void;
}
#endif
