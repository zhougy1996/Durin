#pragma once

#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
namespace Durin::Editor::Level::Testing
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
