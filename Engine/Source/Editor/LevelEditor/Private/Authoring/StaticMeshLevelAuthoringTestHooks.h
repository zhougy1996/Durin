#pragma once

#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
namespace Durin::Editor::Level::Testing
{
	enum class EStaticMeshLevelAuthoringFailurePoint : uint8
	{
		None,
		AfterTemporaryRename,
		AfterRemove,
		AfterCreate,
		AfterFinalRename,
		AfterUpdate,
	};

	auto SetStaticMeshLevelAuthoringFailurePoint(
		EStaticMeshLevelAuthoringFailurePoint Point) -> void;
}
#endif
