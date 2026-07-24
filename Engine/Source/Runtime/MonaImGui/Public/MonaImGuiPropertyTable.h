#pragma once

#include "Math/MathFwd.h"
#include "MonaImGuiAPI.h"
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui::PropertyEdit
{
	struct FWidgetState
	{
		// Compound controls aggregate every child item so callers can treat a
		// vector or transform drag as one continuous property interaction.
		bool bActive = false;
		bool bActivated = false;
		bool bDeactivatedAfterEdit = false;
	};

	struct FTableConfig
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

	MONAIMGUI_API auto BeginTable(const char* Id, const FTableConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EndTable() -> void;
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
	// Edits vector components in one property row. Must be called inside a property table.
	MONAIMGUI_API auto EditVector(const char* Label, FVector2& Value, bool bReadOnly = false, double Speed = 0.05, FWidgetState* OutState = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector3& Value, bool bReadOnly = false, double Speed = 0.05, FWidgetState* OutState = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector4& Value, bool bReadOnly = false, double Speed = 0.05, FWidgetState* OutState = nullptr) -> bool;
	// Presents quaternion storage as Euler degrees and normalizes edited results.
	MONAIMGUI_API auto EditQuat(const char* Label, FQuat& Value, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
	// Draws an expandable Transform property. Must be called inside a property table.
	MONAIMGUI_API auto EditTransform(const char* Label, FTransform& Transform, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
	// Edits linear storage through an sRGB-facing picker. Must be called inside a property table.
	MONAIMGUI_API auto EditColor(const char* Label, FLinearColor& Color, bool bShowAlpha = true, bool bReadOnly = false, FWidgetState* OutState = nullptr) -> bool;
} // namespace Durin::MonaImGui::PropertyEdit
