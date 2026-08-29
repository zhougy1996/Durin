#pragma once

#include "LevelEditorCustomizations.h"
#include "PrimitiveDrawInterface.h"

namespace Durin::Editor::Level
{
	inline auto SubmitXRayVisibleLine(FSceneView& View,
		const FVector3& Start, const FVector3& End, const FVector4f& Color,
		const FSimpleElementLineStyle& Style = {}) -> void
	{
		FViewPrimitiveDrawInterface PDI(View);
		FVector4f ForegroundColor = Color;
		ForegroundColor.w *= 0.32f;
		PDI.DrawTranslucentLine(Start, End, ForegroundColor,
			ESceneDepthPriorityGroup::Foreground, Style);
		if (Color.w < 1.0f)
			PDI.DrawTranslucentLine(Start, End, Color,
				ESceneDepthPriorityGroup::World, Style);
		else
			PDI.DrawLine(Start, End, Color,
				ESceneDepthPriorityGroup::World, Style);
	}

	inline auto SubmitXRayVisibleIcon(FSceneView& View,
		EEditorVisualizationIcon Icon, const FVector3& WorldPosition,
		const FVector4f& Color, float SizePixels) -> void
	{
		constexpr float IconCount = 3.0f;
		const float MinU = static_cast<float>(Icon) / IconCount;
		const float MaxU = (static_cast<float>(Icon) + 1.0f) / IconCount;
		FViewPrimitiveDrawInterface PDI(View);
		FVector4f ForegroundColor = Color;
		ForegroundColor.w *= 0.3f;
		PDI.DrawSprite(WorldPosition, FVector2f(SizePixels),
			FSimpleElementTexture::EditorIconAtlas(), {MinU, 0.0f}, {MaxU, 1.0f},
			ForegroundColor, ESceneDepthPriorityGroup::Foreground);
		PDI.DrawSprite(WorldPosition, FVector2f(SizePixels),
			FSimpleElementTexture::EditorIconAtlas(), {MinU, 0.0f}, {MaxU, 1.0f},
			Color, ESceneDepthPriorityGroup::World);
	}

	inline auto SubmitXRayVisibleWireBox(FSceneView& View,
		const FMatrix& LocalToWorld, const FVector4f& Color,
		float WidthPixels = 1.0f) -> void
	{
		std::array<FVector3, 8> Corners;
		for (uint32 Corner = 0; Corner < Corners.size(); ++Corner)
		{
			Corners[Corner] = FVector3(LocalToWorld * FVector4(
				(Corner & 1) ? 0.5 : -0.5,
				(Corner & 2) ? 0.5 : -0.5,
				(Corner & 4) ? 0.5 : -0.5, 1.0));
		}
		static constexpr std::array<std::array<uint32, 2>, 12> Edges{{
			{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
			{2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
		}};
		for (const auto& Edge : Edges)
			SubmitXRayVisibleLine(View, Corners[Edge[0]], Corners[Edge[1]],
				Color, {.WidthPixels = WidthPixels});
	}
} // namespace Durin::Editor::Level
