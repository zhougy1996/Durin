#pragma once

#include "Math/MathFwd.h"
#include "MonaImGuiAPI.h"
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui
{
	struct FPropertyTableConfig
	{
		const char* PropertyColumnLabel = "Property";
		const char* ValueColumnLabel = "Value";
		float PropertyColumnWeight = 0.32f;
		float ValueColumnWeight = 1.0f;
		float MinimumPropertyColumnWidthInEm = 7.0f;
		float MaximumPropertyColumnWidthInEm = 11.0f;
		ImVec2 CellPadding = {1.0f, 1.0f};
		ImGuiTableFlags Flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBodyUntilResize |
			ImGuiTableFlags_SizingFixedFit;
		bool bShowHeaders = false;
	};

	MONAIMGUI_API auto BeginPropertyTable(const char* Id, const FPropertyTableConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EndPropertyTable() -> void;
	MONAIMGUI_API auto BeginPropertyRow(const char* Label, bool bReadOnly = false, float LabelIndent = 0.0f) -> void;
	MONAIMGUI_API auto EndPropertyRow(bool bReadOnly = false) -> void;
	// Edits vector components in one property row. Must be called inside a property table.
	MONAIMGUI_API auto EditVectorProperty(const char* Label, FVector2& Value, bool bReadOnly = false, double Speed = 0.05) -> bool;
	MONAIMGUI_API auto EditVectorProperty(const char* Label, FVector3& Value, bool bReadOnly = false, double Speed = 0.05) -> bool;
	MONAIMGUI_API auto EditVectorProperty(const char* Label, FVector4& Value, bool bReadOnly = false, double Speed = 0.05) -> bool;
	// Presents quaternion storage as Euler degrees and normalizes edited results.
	MONAIMGUI_API auto EditQuatProperty(const char* Label, FQuat& Value, bool bReadOnly = false) -> bool;
	// Draws an expandable Transform property. Must be called inside a property table.
	MONAIMGUI_API auto EditTransformProperty(const char* Label, FTransform& Transform, bool bReadOnly = false) -> bool;
	// Edits linear storage through an sRGB-facing picker. Must be called inside a property table.
	MONAIMGUI_API auto EditColorProperty(const char* Label, FLinearColor& Color, bool bShowAlpha = true, bool bReadOnly = false) -> bool;
} // namespace Durin::MonaImGui
