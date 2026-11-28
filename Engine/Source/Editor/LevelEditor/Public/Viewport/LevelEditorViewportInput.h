#pragma once

#include "CoreMinimal.h"

namespace Durin::LevelEditor
{
	// Captures one frame of normalized viewport navigation and gizmo input.
	struct FLevelEditorViewportInput
	{
		float DeltaSeconds = 0.0f;
		FVector2f MouseDelta{0.0f};
		float MouseWheel = 0.0f;
		bool bHovered = false;
		bool bFocused = false;
		bool bWantTextInput = false;
		bool bAlt = false;
		bool bShift = false;
		bool bCtrl = false;
		bool bLeftMouseDown = false;
		bool bMiddleMouseDown = false;
		bool bRightMouseDown = false;
		bool bLeftMousePressed = false;
		bool bLeftMouseDoubleClicked = false;
		bool bMiddleMousePressed = false;
		bool bRightMousePressed = false;
		bool bMoveForward = false;
		bool bMoveBackward = false;
		bool bMoveLeft = false;
		bool bMoveRight = false;
		bool bMoveDown = false;
		bool bMoveUp = false;
		bool bFocusSelection = false;
		bool bRequestSelection = false;
		bool bCancel = false;
		bool bModeTranslate = false;
		bool bModeRotate = false;
		bool bModeScale = false;
		bool bDelete = false;
		bool bDuplicate = false;
		bool bAppend = false;
		FVector2f MousePosition{0.0f};
		FVector2f ViewportSize{0.0f};
	};
}
