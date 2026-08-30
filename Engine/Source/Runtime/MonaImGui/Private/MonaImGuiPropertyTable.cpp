#include "MonaImGuiPropertyTable.h"

#include "Math/Color.h"
#include "Math/Operations.h"
#include "Math/Transform.h"
#include "MonaImGui.h"

namespace Durin::MonaImGui::PropertyEdit
{
	namespace
	{
		auto AccumulateLastItemState(FWidgetState* State) -> void
		{
			if (!State) return;
			State->bActive |= ImGui::IsItemActive();
			State->bActivated |= ImGui::IsItemActivated();
			State->bDeactivatedAfterEdit |= ImGui::IsItemDeactivatedAfterEdit();
		}

		template<size_t NumComponents, typename TVector>
		auto EditComponentValues(
			const char* Id,
			TVector& Value,
			double Speed,
			FWidgetState* State,
			const FValueWidgetConfig& Config
		) -> bool
		{
			static_assert(NumComponents >= 2 && NumComponents <= 4);
			using TScalar = typename TVector::value_type;
			static_assert(std::is_same_v<TScalar, float> || std::is_same_v<TScalar, double>);
			constexpr ImGuiDataType DataType = std::is_same_v<TScalar, float>
				? ImGuiDataType_Float : ImGuiDataType_Double;
			const TScalar MinimumValue = static_cast<TScalar>(Config.MinimumValue);
			const TScalar MaximumValue = static_cast<TScalar>(Config.MaximumValue);
			const std::array<ImVec4, 4> ComponentColors = {
				GetThemeColor(EUIThemeColor::AxisX),
				GetThemeColor(EUIThemeColor::AxisY),
				GetThemeColor(EUIThemeColor::AxisZ),
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
			};
			const char* Format = Config.Format ? Config.Format : "%.3f";
			const float IndicatorWidth = ScaleUI(2.0f);
			const float IndicatorInset = std::max(ImGui::GetStyle().FrameRounding * 0.5f, IndicatorWidth);

			ImGui::PushID(Id);
			bool bChanged = false;
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ScaleUI(3.0f), 0.0f));
			const float MaximumWidth = Config.MaximumWidthInEm > 0.0f
				? ImGui::GetFontSize() * Config.MaximumWidthInEm : 0.0f;
			const ImVec2 OuterSize = MaximumWidth > 0.0f
				? ImVec2(std::min(ImGui::GetContentRegionAvail().x, MaximumWidth), 0.0f) : ImVec2{};
			if (ImGui::BeginTable("##Components", static_cast<int>(NumComponents),
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX,
				OuterSize))
			{
				for (size_t Component = 0; Component < NumComponents; ++Component)
				{
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushID(static_cast<int>(Component));
					bChanged |= ImGui::DragScalar(
						"##Value",
						DataType,
						&Value[Component],
						static_cast<float>(Speed),
						Config.bHasRange ? &MinimumValue : nullptr,
						Config.bHasRange ? &MaximumValue : nullptr,
						Format,
						Config.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
					AccumulateLastItemState(State);
					const ImVec2 FrameMin = ImGui::GetItemRectMin();
					const ImVec2 FrameMax = ImGui::GetItemRectMax();
					ImGui::GetWindowDrawList()->AddLine(
						{FrameMin.x + IndicatorWidth * 0.5f, FrameMin.y + IndicatorInset},
						{FrameMin.x + IndicatorWidth * 0.5f, FrameMax.y - IndicatorInset},
						ImGui::GetColorU32(ComponentColors[Component]),
						IndicatorWidth);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::PopStyleVar();
			ImGui::PopID();
			return bChanged;
		}

		template<typename TVector>
		auto EditVectorRow(const char* Label, TVector& Value, bool bReadOnly, double Speed,
			FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
		{
			BeginRow(Label, bReadOnly, 0.0f, LabelTooltip);
			const bool bChanged = EditVectorValue(Label, Value, Speed, OutState, Config);
			EndRow(bReadOnly);
			return bChanged;
		}

		auto QuatToEulerDegrees(const FQuat& Value) -> FVector3
		{
			// Invalid quaternion storage should remain recoverable from the editor instead of producing NaN controls.
			const FQuat Normalized = Math::LengthSquared(Value) > 1.e-12
				? Math::Normalize(Value) : FQuatConstants::Identity;
			return Math::QuaternionToEulerDegrees(Normalized);
		}

		auto EulerDegreesToQuat(const FVector3& Value) -> FQuat
		{
			return Math::Normalize(Math::MakeQuaternionFromEulerDegrees(Value));
		}
	} // namespace

	auto BeginTable(const char* Id, const FTableConfig& Config) -> bool
	{
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Config.CellPadding);
		if (!ImGui::BeginTable(Id, 2, Config.Flags))
		{
			ImGui::PopStyleVar();
			return false;
		}
		const float FontSize = ImGui::GetFontSize();
		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const float DesiredPropertyWidth = AvailableWidth * Config.PropertyColumnWeight;
		const float PropertyWidth = std::clamp(
			DesiredPropertyWidth,
			FontSize * Config.MinimumPropertyColumnWidthInEm,
			FontSize * Config.MaximumPropertyColumnWidthInEm
		);
		ImGui::TableSetupColumn(Config.PropertyColumnLabel, ImGuiTableColumnFlags_WidthFixed, PropertyWidth);
		if (Config.MaximumValueColumnWidthInEm > 0.0f)
		{
			const float ValueWidth = std::min(
				FontSize * Config.MaximumValueColumnWidthInEm,
				std::max(AvailableWidth - PropertyWidth, 0.0f));
			ImGui::TableSetupColumn(Config.ValueColumnLabel, ImGuiTableColumnFlags_WidthFixed, ValueWidth);
		}
		else
		{
			ImGui::TableSetupColumn(Config.ValueColumnLabel, ImGuiTableColumnFlags_WidthStretch, Config.ValueColumnWeight);
		}
		if (Config.bShowHeaders) ImGui::TableHeadersRow();
		return true;
	}

	auto EditVectorValue(const char* Id, FVector2& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<2>(Id, Value, Speed, OutState, Config);
	}

	auto EditVectorValue(const char* Id, FVector2f& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<2>(Id, Value, Speed, OutState, Config);
	}

	auto EditVectorValue(const char* Id, FVector3f& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<3>(Id, Value, Speed, OutState, Config);
	}

	auto EditVectorValue(const char* Id, FVector4f& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<4>(Id, Value, Speed, OutState, Config);
	}

	auto EditVectorValue(const char* Id, FVector3& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<3>(Id, Value, Speed, OutState, Config);
	}

	auto EditVectorValue(const char* Id, FVector4& Value, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config) -> bool
	{
		return EditComponentValues<4>(Id, Value, Speed, OutState, Config);
	}

	auto EditVector(const char* Label, FVector2& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditVector(const char* Label, FVector3& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditVector(const char* Label, FVector4& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditVector(const char* Label, FVector2f& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditVector(const char* Label, FVector3f& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditVector(const char* Label, FVector4f& Value, bool bReadOnly, double Speed,
		FWidgetState* OutState, const FValueWidgetConfig& Config, const char* LabelTooltip) -> bool
	{
		return EditVectorRow(Label, Value, bReadOnly, Speed, OutState, Config, LabelTooltip);
	}

	auto EditQuat(const char* Label, FQuat& Value, bool bReadOnly, FWidgetState* OutState,
		const char* LabelTooltip) -> bool
	{
		FVector3 RotationDegrees = QuatToEulerDegrees(Value);
		if (!EditVector(Label, RotationDegrees, bReadOnly, 0.25, OutState, {}, LabelTooltip)) return false;
		Value = EulerDegreesToQuat(RotationDegrees);
		return true;
	}

	auto EndTable() -> void
	{
		ImGui::EndTable();
		ImGui::PopStyleVar();
	}

	auto BeginTreeRow(const char* Id, const char* Label, ImGuiTreeNodeFlags Flags) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		return CompactTreeNode(Id, Flags | ImGuiTreeNodeFlags_FramePadding, "%s", Label);
	}

	auto EndTreeRow() -> void
	{
		ImGui::TreePop();
	}

	auto BeginGroup(const char* Id, const char* Label, ImGuiTreeNodeFlags Flags) -> bool
	{
		return BeginTreeRow(Id, Label,
			Flags | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns);
	}

	auto EndGroup() -> void
	{
		EndTreeRow();
	}

	auto BeginFixedArray(const char* Id, const char* Label, uint64 Count, ImGuiTreeNodeFlags Flags) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		return CompactTreeNode(Id,
			Flags | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding,
			"%s (%llu)", Label, Count);
	}

	auto EndFixedArray() -> void
	{
		ImGui::TreePop();
	}

	auto BeginFixedArrayElement(const char* Label, ImGuiTreeNodeFlags Flags) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		return CompactTreeNode(Label,
			Flags | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding);
	}

	auto EndFixedArrayElement() -> void
	{
		ImGui::TreePop();
	}

	auto BeginRow(const char* Label, bool bReadOnly, float LabelIndent, const char* LabelTooltip) -> void
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		// Plain rows share the tree label column rather than starting under its disclosure arrow.
		const float EffectiveLabelIndent = LabelIndent > 0.0f ? LabelIndent : GetCompactTreeNodeToLabelSpacing();
		ImGui::Indent(EffectiveLabelIndent);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Label);
		ShowLabelTooltip(LabelTooltip, bReadOnly);
		ImGui::Unindent(EffectiveLabelIndent);
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	auto EndRow(bool bReadOnly) -> void
	{
		if (bReadOnly) ImGui::EndDisabled();
	}

	auto ShowLabelTooltip(const char* LabelTooltip, bool bReadOnly) -> void
	{
		if ((!LabelTooltip || LabelTooltip[0] == '\0') && !bReadOnly) return;
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
		ImGui::BeginTooltip();
		if (LabelTooltip && LabelTooltip[0] != '\0') ImGui::TextUnformatted(LabelTooltip);
		if (bReadOnly)
		{
			if (LabelTooltip && LabelTooltip[0] != '\0') ImGui::Separator();
			ImGui::TextDisabled("Read-only");
		}
		ImGui::EndTooltip();
	}

	auto EditTransform(const char* Label, FTransform& Transform, bool bReadOnly, FWidgetState* OutState,
		const char* LabelTooltip) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = CompactTreeNode(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding);
		ShowLabelTooltip(LabelTooltip, bReadOnly);
		if (!bOpen) return false;

		auto EditTransformRow = [&](const char* RowLabel, FVector3& Value, double Speed) -> bool {
			BeginRow(RowLabel, bReadOnly, GetCompactTreeNodeToLabelSpacing(), "Type: Vector3");
			const bool bChanged = EditVectorValue(RowLabel, Value, Speed, OutState);
			EndRow(bReadOnly);
			return bChanged;
		};

		bool bChanged = EditTransformRow("Location", Transform.Translation, 0.05);
		FVector3 RotationDegrees = QuatToEulerDegrees(Transform.Rotation);
		if (EditTransformRow("Rotation", RotationDegrees, 0.25))
		{
			Transform.Rotation = EulerDegreesToQuat(RotationDegrees);
			bChanged = true;
		}
		bChanged |= EditTransformRow("Scale", Transform.Scale3D, 0.01);
		ImGui::TreePop();
		return bChanged;
	}

	auto EditColor(const char* Label, FLinearColor& Color, bool bShowAlpha, bool bReadOnly,
		FWidgetState* OutState, const char* LabelTooltip) -> bool
	{
		ImGui::PushID(Label);
		BeginRow(Label, bReadOnly, 0.0f, LabelTooltip);
		FLinearColor DisplayColor(ColorConvert::LinearToSRGB(Color.R), ColorConvert::LinearToSRGB(Color.G), ColorConvert::LinearToSRGB(Color.B), std::clamp(Color.A, 0.0f, 1.0f));
		const ImGuiColorEditFlags Flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB;
		bool bChanged = false;

		// Match details panels that treat color as a swatch rather than an inline
		// vector. Precise channel editing remains available in the picker.
		const ImGuiColorEditFlags PreviewFlags = bShowAlpha
			? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha;
		const ImVec2 PreviewSize(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
		if (ImGui::ColorButton("##Value", ImVec4(DisplayColor.R, DisplayColor.G, DisplayColor.B, DisplayColor.A),
			PreviewFlags, PreviewSize))
			ImGui::OpenPopup("##Picker");
		if (ImGui::BeginPopup("##Picker"))
		{
			bChanged |= bShowAlpha
				? ImGui::ColorPicker4("##PickerValue", DisplayColor.RGBA, Flags | ImGuiColorEditFlags_AlphaBar)
				: ImGui::ColorPicker3("##PickerValue", DisplayColor.RGBA, Flags);
			AccumulateLastItemState(OutState);
			ImGui::EndPopup();
		}
		if (bChanged)
		{
			Color.R = ColorConvert::SRGBToLinear(DisplayColor.R);
			Color.G = ColorConvert::SRGBToLinear(DisplayColor.G);
			Color.B = ColorConvert::SRGBToLinear(DisplayColor.B);
			if (bShowAlpha) Color.A = DisplayColor.A;
		}
		EndRow(bReadOnly);
		ImGui::PopID();
		return bChanged;
	}
} // namespace Durin::MonaImGui::PropertyEdit
