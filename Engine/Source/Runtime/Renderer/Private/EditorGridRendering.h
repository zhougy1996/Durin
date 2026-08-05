#pragma once

#include "SceneView.h"
#include "RendererAPI.h"

namespace Durin::EditorGridRendering
{
	// Mirrors the editor-grid shader uniform layout uploaded for each view.
	struct FEditorGridUniform
	{
		FMatrix4f WorldToClip{1.0f};
		FMatrix4f ClipToWorld{1.0f};
		FVector4f GridPlane{0.0f};
		FVector4f ViewPositionFadeDistance{0.0f};
		FVector4f MinorColor{1.0f};
		FVector4f MajorColor{1.0f};
		FVector4f AxisXColor{1.0f};
		FVector4f AxisYColor{1.0f};
	};

	static_assert(offsetof(FEditorGridUniform, WorldToClip) == 0);
	static_assert(offsetof(FEditorGridUniform, ClipToWorld) == 64);
	static_assert(offsetof(FEditorGridUniform, GridPlane) == 128);
	static_assert(offsetof(FEditorGridUniform, ViewPositionFadeDistance) == 144);
	static_assert(offsetof(FEditorGridUniform, MinorColor) == 160);
	static_assert(offsetof(FEditorGridUniform, MajorColor) == 176);
	static_assert(offsetof(FEditorGridUniform, AxisXColor) == 192);
	static_assert(offsetof(FEditorGridUniform, AxisYColor) == 208);
	static_assert(sizeof(FEditorGridUniform) == 224);

	RENDERER_API auto BuildUniform(const FSceneView& View, FEditorGridUniform& OutUniform) -> bool;
}
