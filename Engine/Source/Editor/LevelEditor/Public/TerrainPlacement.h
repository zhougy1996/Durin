#pragma once

#include "DObject/ObjectPtr.h"
#include "LevelEditorAPI.h"
#include "Math/Transform.h"
#include "Misc/Name.h"

namespace Durin::Editor { class FTransactionManager; }

namespace Durin
{
	class ATerrainActor;
	class DLevel;
	class DPackage;
	class DTerrainHeightmap;
}

namespace Durin::Editor::Level
{
	enum class ETerrainPlacementError : uint8
	{
		None,
		InvalidRequest,
		WrongThread,
		ReadOnly,
		StaleTarget,
		NameConflict,
		UnavailableHeightmap,
		InvalidProperties,
		ExecutionFailed,
	};

	struct FTerrainPlacementRequest
	{
		DLevel* Level = nullptr;
		std::string ExpectedPackagePath;
		uint64 ExpectedPackageEditRevision = 0;
		bool bReadOnly = false;
		FName ActorName;
		TObjectPtr<DTerrainHeightmap> Heightmap;
		uint64 ExpectedHeightmapRevision = 0;
		FTransform Transform;
		double SpacingX = 100.0;
		double SpacingY = 100.0;
		double HeightScale = 1000.0;
		double HeightOffset = 0.0;
		bool bVisible = true;
		std::string Description = "Place terrain actor";
	};

	struct FTerrainPlacementDiagnostic
	{
		ETerrainPlacementError Error = ETerrainPlacementError::None;
		std::string Message;
		explicit operator bool() const { return Error == ETerrainPlacementError::None; }
	};

	struct FTerrainPlacementPlan
	{
		TObjectPtr<DLevel> Level;
		TObjectPtr<DPackage> Package;
		std::string PackagePath;
		uint64 PackageEditRevision = 0;
		uint64 ActorHierarchyRevision = 0;
		FName ActorName;
		TObjectPtr<DTerrainHeightmap> Heightmap;
		uint64 HeightmapRevision = 0;
		FTransform Transform;
		double SpacingX = 100.0;
		double SpacingY = 100.0;
		double HeightScale = 1000.0;
		double HeightOffset = 0.0;
		bool bVisible = true;
		std::string Description;
		FTerrainPlacementDiagnostic Diagnostic;
		explicit operator bool() const { return static_cast<bool>(Diagnostic); }
	};

	struct FTerrainPlacementResult
	{
		FTerrainPlacementDiagnostic Diagnostic;
		TObjectPtr<ATerrainActor> Actor;
		bool bChanged = false;
		explicit operator bool() const { return static_cast<bool>(Diagnostic); }
	};

	struct FTerrainPlacementExecutionContext
	{
		DLevel* OpenLevel = nullptr;
		::Durin::Editor::FTransactionManager* Transactions = nullptr;
		bool bReadOnly = false;
	};

	// Captures, validates, and atomically executes one finite Terrain placement.
	class LEVELEDITOR_API FTerrainPlacement
	{
	public:
		static auto CaptureTarget(DLevel& Level) -> FTerrainPlacementRequest;
		static auto Plan(const FTerrainPlacementRequest& Request) -> FTerrainPlacementPlan;
		static auto Execute(const FTerrainPlacementPlan& Plan,
			const FTerrainPlacementExecutionContext& Context) -> FTerrainPlacementResult;
	};
}
