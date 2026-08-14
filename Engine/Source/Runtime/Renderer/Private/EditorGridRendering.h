#pragma once

#include "SceneView.h"
#include "RendererAPI.h"

namespace Durin::EditorGridRendering
{
	inline constexpr int32 MinimumGridExponent = -4;
	inline constexpr int32 MaximumGridExponent = 8;
	inline constexpr uint32 GridPhaseCount =
		MaximumGridExponent - MinimumGridExponent + 1;

	// Mirrors the editor-grid shader uniform layout uploaded for each view.
	struct FEditorGridUniform
	{
		FMatrix4f RelativeWorldToClip{1.0f};
		FMatrix4f ClipToRelativeWorld{1.0f};
		FVector4f GridPlane{0.0f};
		FVector4f ViewPositionFadeDistance{0.0f};
		std::array<FVector4f, GridPhaseCount> GridPhases{};
		FVector4f MinorColor{1.0f};
		FVector4f MajorColor{1.0f};
		FVector4f AxisXColor{1.0f};
		FVector4f AxisYColor{1.0f};
		FVector4f ClipPlane{0.0f};
	};

	static_assert(offsetof(FEditorGridUniform, RelativeWorldToClip) == 0);
	static_assert(offsetof(FEditorGridUniform, ClipToRelativeWorld) == 64);
	static_assert(offsetof(FEditorGridUniform, GridPlane) == 128);
	static_assert(offsetof(FEditorGridUniform, ViewPositionFadeDistance) == 144);
	static_assert(offsetof(FEditorGridUniform, GridPhases) == 160);
	static_assert(offsetof(FEditorGridUniform, MinorColor) == 368);
	static_assert(offsetof(FEditorGridUniform, MajorColor) == 384);
	static_assert(offsetof(FEditorGridUniform, AxisXColor) == 400);
	static_assert(offsetof(FEditorGridUniform, AxisYColor) == 416);
	static_assert(offsetof(FEditorGridUniform, ClipPlane) == 432);
	static_assert(sizeof(FEditorGridUniform) == 448);

	RENDERER_API auto BuildUniform(const FSceneView& View, FEditorGridUniform& OutUniform) -> bool;
}
