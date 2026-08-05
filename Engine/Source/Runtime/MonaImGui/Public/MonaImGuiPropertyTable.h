#pragma once

#include "Math/MathFwd.h"
#include "MonaImGuiAPI.h"
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui::PropertyEdit
{
	// Aggregates ImGui activation transitions for one logical property control.
	struct FWidgetState
	{
		// Compound controls aggregate every child item so callers can treat a
		// vector or transform drag as one continuous property interaction.
		bool bActive = false;
		bool bActivated = false;
		bool bDeactivatedAfterEdit = false;
	};

	// Defines labels, sizing, and table flags shared by property editors.
	struct FTableConfig
	{
		const char* PropertyColumnLabel = "Property";
		const char* ValueColumnLabel = "Value";
		float PropertyColumnWeight = 0.32f;
		float ValueColumnWeight = 1.0f;
		float MinimumPropertyColumnWidthInEm = 7.0f;
		float MaximumPropertyColumnWidthInEm = 11.0f;
		// Zero preserves the default behavior where the value column consumes all
		// remaining table width.
		float MaximumValueColumnWidthInEm = 0.0f;
		ImVec2 CellPadding = {1.0f, 1.0f};
		ImGuiTableFlags Flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBodyUntilResize |
			ImGuiTableFlags_SizingFixedFit;
		bool bShowHeaders = false;
	};

	// Applies optional presentation constraints to a value widget without
	// changing its underlying data type.
	struct FValueWidgetConfig
	{
		float MaximumWidthInEm = 0.0f;
		bool bHasRange = false;
		double MinimumValue = 0.0;
		double MaximumValue = 0.0;
		const char* Format = nullptr;
	};

	MONAIMGUI_API auto BeginTable(const char* Id, const FTableConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EndTable() -> void;
	// Draws an expandable group spanning the property and value columns. Call
	// EndGroup() only when BeginGroup() returns true.
	MONAIMGUI_API auto BeginGroup(const char* Id, const char* Label,
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_DefaultOpen) -> bool;
	MONAIMGUI_API auto EndGroup() -> void;
	// Draws an expandable array-shaped group without structural edit controls.
	// Call EndFixedArray() only when BeginFixedArray() returns true.
	MONAIMGUI_API auto BeginFixedArray(const char* Id, const char* Label, uint64 Count,
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_DefaultOpen) -> bool;
	MONAIMGUI_API auto EndFixedArray() -> void;
	// Draws one expandable element inside a fixed-array group. Call
	// EndFixedArrayElement() only when BeginFixedArrayElement() returns true.
	MONAIMGUI_API auto BeginFixedArrayElement(const char* Label,
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_DefaultOpen) -> bool;
	MONAIMGUI_API auto EndFixedArrayElement() -> void;
	MONAIMGUI_API auto BeginRow(const char* Label, bool bReadOnly = false, float LabelIndent = 0.0f) -> void;
	MONAIMGUI_API auto EndRow(bool bReadOnly = false) -> void;
	// Edits vector components in the current value column without opening a row.
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector2& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector3& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector4& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	// Edits vector components in one property row. Must be called inside a property table.
	MONAIMGUI_API auto EditVector(const char* Label, FVector2& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector3& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector4& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	// Presents quaternion storage as Euler degrees and normalizes edited results.
	MONAIMGUI_API auto EditQuat(const char* Label, FQuat& Value, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
	// Draws an expandable Transform property. Must be called inside a property table.
	MONAIMGUI_API auto EditTransform(const char* Label, FTransform& Transform, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
	// Edits linear storage through an sRGB-facing picker. Must be called inside a property table.
	MONAIMGUI_API auto EditColor(const char* Label, FLinearColor& Color, bool bShowAlpha = true, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
} // namespace Durin::MonaImGui::PropertyEdit
