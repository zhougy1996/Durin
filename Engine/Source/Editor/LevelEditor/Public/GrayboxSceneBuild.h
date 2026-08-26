#pragma once

#include "LevelEditorAPI.h"
#include "Math/Box.h"
#include "Math/Transform.h"
#include "Misc/Name.h"

namespace Durin::Editor::Level
{
	struct FGrayboxOpenArenaParams
	{
		double Width = 20.0;
		double Depth = 20.0;
		double FloorThickness = 0.5;
		double WallHeight = 4.0;
		double WallThickness = 0.5;
		bool bCeiling = false;
	};

	struct FGrayboxArenaPiece
	{
		FName Name;
		FTransform Transform;
	};

	struct FGrayboxOpenArenaLayout
	{
		std::vector<FGrayboxArenaPiece> Pieces;
		FTransform PlayerStartTransform;
		FTransform DirectionalLightTransform;
	};

	// Converts clear interior dimensions into Box transforms using actual mesh bounds.
	LEVELEDITOR_API auto BuildGrayboxOpenArenaLayout(
		const FGrayboxOpenArenaParams& Params,
		const FBox& BoxLocalBounds,
		FGrayboxOpenArenaLayout& OutLayout,
		std::string& OutError) -> bool;

	// Registered by LevelEditor as the repository-native startup command handler.
	LEVELEDITOR_API auto RunGrayboxBuildStartupCommand(
		std::span<const std::string> Arguments) -> int;
}
