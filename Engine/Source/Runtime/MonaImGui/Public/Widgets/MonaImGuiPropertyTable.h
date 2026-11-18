#pragma once

#include "CoreMinimal.h"

#include "Math/MathFwd.h"
#include "MonaImGuiAPI.h"
#include "ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui::PropertyEdit
{
	// Non-owning tooltip source, resolved only while its label is hovered.
	struct FLabelTooltip
	{
		using FBuilder = std::string (*)(const void*);
		FLabelTooltip(const char* InText = nullptr) : Text(InText) {}
		FLabelTooltip(const void* InContext, FBuilder InBuilder) : Context(InContext), Builder(InBuilder) {}
		const char* Text = nullptr;
		const void* Context = nullptr;
		FBuilder Builder = nullptr;
	};

	// Aggregates ImGui activation transitions for one logical property control.
	struct FWidgetState
	{
		// Compound controls aggregate every child item so callers can treat a
		// vector or transform drag as one continuous property interaction.
		bool bActive = false;
		bool bActivated = false;
		bool bDeactivatedAfterEdit = false;
	};

	enum class EPropertyInteractionEnd { None, Commit, Cancel };
	enum class EPropertyInteractionBoundary { Widget, Popup };
	struct FPropertyInteractionState
	{
		ImGuiID Id = 0;
		EPropertyInteractionBoundary Boundary = EPropertyInteractionBoundary::Widget;
		EPropertyInteractionEnd End = EPropertyInteractionEnd::None;
		static MONAIMGUI_API auto FromWidget(ImGuiID Id, const FWidgetState& State, bool bReadOnly = false) -> FPropertyInteractionState;
		static MONAIMGUI_API auto FromPopup(ImGuiID Id, bool bOpen, bool bReadOnly = false) -> FPropertyInteractionState;
		MONAIMGUI_API auto IsCancelled() const -> bool;
	};
	enum class EColorPickerMode { SRGB, Linear, LinearHDR };

	class FPropertyEditInteraction
	{
	public:
		MONAIMGUI_API auto Observe(const FPropertyInteractionState& State) -> void;
		MONAIMGUI_API auto Poll(bool bEndOfFrame = false) -> EPropertyInteractionEnd;
		auto Matches(const FPropertyInteractionState& State) const -> bool { return IsActive() && State.Id == Current.Id && State.Boundary == Current.Boundary; }
		auto IsActive() const -> bool { return Current.Id != 0; }
		auto RequestCancel() -> void { Current.End = EPropertyInteractionEnd::Cancel; }
		auto IsCancelPending() const -> bool { return Current.End == EPropertyInteractionEnd::Cancel; }
		MONAIMGUI_API auto CanRetryFinish() const -> bool;
		MONAIMGUI_API auto RecordFailure(std::string_view Message) -> bool;

	private:
		FPropertyInteractionState Current;
		int LastDrawFrame = 0;
		int FailedFinishFrame = -1;
		std::string LastError;
	};

	MONAIMGUI_API auto DrawColorPicker(FLinearColor& Color, EColorPickerMode Mode,
		bool bShowAlpha, bool bReadOnly, FPropertyInteractionState* OutPickerState,
		FWidgetState* OutState = nullptr) -> bool;

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
	// Draws an expandable property-table row with the same vertical alignment as
	// framed value widgets. Call EndTreeRow() only when BeginTreeRow() returns true.
	MONAIMGUI_API auto BeginTreeRow(const char* Id, const char* Label,
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_DefaultOpen) -> bool;
	MONAIMGUI_API auto EndTreeRow() -> void;
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
	MONAIMGUI_API auto BeginRow(const char* Label, bool bReadOnly = false, float LabelIndent = 0.0f,
		FLabelTooltip LabelTooltip = nullptr) -> void;
	MONAIMGUI_API auto EndRow(bool bReadOnly = false) -> void;
	// Shows delayed metadata for the most recently drawn property label.
	MONAIMGUI_API auto ShowLabelTooltip(FLabelTooltip LabelTooltip, bool bReadOnly = false) -> void;
	// Edits vector components in the current value column without opening a row.
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector2f& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector3f& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector4f& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector2& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector3& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	MONAIMGUI_API auto EditVectorValue(const char* Id, FVector4& Value, double Speed = 0.05,
		FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {}) -> bool;
	// Edits vector components in one property row. Must be called inside a property table.
	MONAIMGUI_API auto EditVector(const char* Label, FVector2f& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector3f& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector4f& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector2& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector3& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	MONAIMGUI_API auto EditVector(const char* Label, FVector4& Value, bool bReadOnly = false,
		double Speed = 0.05, FWidgetState* OutState = nullptr, const FValueWidgetConfig& Config = {},
		FLabelTooltip LabelTooltip = nullptr) -> bool;
	// Presents quaternion storage as Euler degrees and normalizes edited results.
	MONAIMGUI_API auto EditQuat(const char* Label, FQuat& Value, bool bReadOnly = false,
		FWidgetState* OutState = nullptr, FLabelTooltip LabelTooltip = nullptr) -> bool;
	// Draws Transform editing rows with an optional expandable header inside a property table.
	MONAIMGUI_API auto EditTransform(const char* Label, FTransform& Transform, bool bReadOnly = false,
		FWidgetState* OutState = nullptr, FLabelTooltip LabelTooltip = nullptr, bool bShowHeader = true) -> bool;
	// Edits linear storage through an sRGB-facing picker. Must be called inside a property table.
	MONAIMGUI_API auto EditColor(const char* Label, FLinearColor& Color, bool bShowAlpha = true,
		bool bReadOnly = false, FWidgetState* OutState = nullptr, FLabelTooltip LabelTooltip = nullptr,
		FPropertyInteractionState* OutPickerState = nullptr) -> bool;
} // namespace Durin::MonaImGui::PropertyEdit
