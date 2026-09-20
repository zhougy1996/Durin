#pragma once

#include "DObject/ObjectPtr.h"
#include "LevelEditorAPI.h"
#include "Math/Transform.h"
#include "Misc/Name.h"

namespace Durin
{
	class DTransactor;
}

namespace Durin
{
	class AStaticMeshActor;
	class DLevel;
	class DPackage;
	class DStaticMesh;
}

namespace Durin::Editor::Level
{

	enum class EStaticMeshLevelMutationKind : uint8
	{
		Create,
		Update,
		Rename,
		Remove,
	};

	enum class EStaticMeshLevelMutationError : uint8
	{
		None,
		InvalidRequest,
		WrongThread,
		ReadOnly,
		StaleTarget,
		UnsupportedActor,
		MissingActor,
		NameConflict,
		InvalidTransform,
		ExecutionFailed,
	};

	struct FStaticMeshActorMutationState
	{
		FName Name;
		TObjectPtr<DStaticMesh> StaticMesh;
		FTransform Transform;
		bool bHidden = false;
	};

	struct FStaticMeshLevelMutation
	{
		EStaticMeshLevelMutationKind Kind = EStaticMeshLevelMutationKind::Create;
		FName TargetName;
		FStaticMeshActorMutationState Desired;
	};

	struct FStaticMeshLevelMutationRequest
	{
		DLevel* Level = nullptr;
		std::string ExpectedPackagePath;
		uint64 ExpectedPackageEditRevision = 0;
		bool bReadOnly = false;
		std::string Description = "Edit static mesh actors";
		std::vector<FStaticMeshLevelMutation> Mutations;
	};

	struct FStaticMeshActorMutationDelta
	{
		std::optional<FStaticMeshActorMutationState> Before;
		std::optional<FStaticMeshActorMutationState> After;
	};

	// Error controls flow; Message is ready for UI, and MutationIndex identifies a failed item.
	struct FStaticMeshLevelMutationPlan
	{
		TObjectPtr<DLevel> Level;
		TObjectPtr<DPackage> Package;
		std::string PackagePath;
		uint64 PackageEditRevision = 0;
		uint64 ActorHierarchyRevision = 0;
		std::string Description;
		std::vector<FStaticMeshActorMutationDelta> Deltas;
		EStaticMeshLevelMutationError Error = EStaticMeshLevelMutationError::None;
		std::string Message;
		// max() means the failure does not identify a request item.
		size_t MutationIndex = std::numeric_limits<size_t>::max();
		bool bHasChanges = false;

		explicit operator bool() const { return Error == EStaticMeshLevelMutationError::None; }
	};

	struct FStaticMeshLevelMutationResult
	{
		EStaticMeshLevelMutationError Error = EStaticMeshLevelMutationError::None;
		std::string Message;
		// max() means the failure does not identify a request item.
		size_t MutationIndex = std::numeric_limits<size_t>::max();
		std::vector<FName> ResultActorNames;
		bool bChanged = false;

		explicit operator bool() const { return Error == EStaticMeshLevelMutationError::None; }
	};

	struct FStaticMeshLevelExecutionContext
	{
		DLevel* OpenLevel = nullptr;
		::Durin::DTransactor* Transactions = nullptr;
		bool bReadOnly = false;
	};

	// Plans and applies bounded structural edits for ordinary, unattached
	// AStaticMeshActor graphs. Planning never mutates the Level.
	class LEVELEDITOR_API FStaticMeshLevelMutations
	{
	public:
		static auto CaptureTarget(DLevel& Level) -> FStaticMeshLevelMutationRequest;
		static auto Plan(const FStaticMeshLevelMutationRequest& Request) -> FStaticMeshLevelMutationPlan;
		static auto Execute(
			const FStaticMeshLevelMutationPlan& Plan,
			const FStaticMeshLevelExecutionContext& Context) -> FStaticMeshLevelMutationResult;
		static auto IsSupportedActor(const AStaticMeshActor& Actor) -> bool;
	};
}
