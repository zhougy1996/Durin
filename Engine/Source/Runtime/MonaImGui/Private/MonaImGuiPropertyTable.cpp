#include "MonaImGuiPropertyTable.h"

#include "Math/Color.h"
#include "Math/Transform.h"
#include "MonaImGui.h"

namespace Durin::MonaImGui
{
	namespace
	{
		auto EditAxisValues(const char* Id, FVector3& Value, double Speed) -> bool
		{
			static constexpr std::array<const char*, 3> AxisNames = {"X", "Y", "Z"};
			const std::array<ImVec4, 3> AxisColors = {
				GetThemeColor(EUIThemeColor::AxisX),
				GetThemeColor(EUIThemeColor::AxisY),
				GetThemeColor(EUIThemeColor::AxisZ)
			};

			ImGui::PushID(Id);
			bool bChanged = false;
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ScaleUI(3.0f), 0.0f));
			if (ImGui::BeginTable("##Axes", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
			{
				for (int Axis = 0; Axis < 3; ++Axis)
				{
					ImGui::TableNextColumn();
					ImGui::AlignTextToFramePadding();
					ImGui::TextColored(AxisColors[Axis], "%s", AxisNames[Axis]);
					ImGui::SameLine(0.0f, ScaleUI(3.0f));
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushID(Axis);
					bChanged |= ImGui::DragScalar("##Value", ImGuiDataType_Double, &Value[Axis], static_cast<float>(Speed));
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::PopStyleVar();
			ImGui::PopID();
			return bChanged;
		}

		auto LinearToSRGB(float Value) -> float
		{
			const float Linear = std::clamp(Value, 0.0f, 1.0f);
			return Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * std::pow(Linear, 1.0f / 2.4f) - 0.055f;
		}

		auto SRGBToLinear(float Value) -> float
		{
			const float SRGB = std::clamp(Value, 0.0f, 1.0f);
			return SRGB <= 0.04045f ? SRGB / 12.92f : std::pow((SRGB + 0.055f) / 1.055f, 2.4f);
		}
	} // namespace

	auto BeginPropertyTable(const char* Id, const FPropertyTableConfig& Config) -> bool
	{
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Config.CellPadding);
		if (!ImGui::BeginTable(Id, 2, Config.Flags))
		{
			ImGui::PopStyleVar();
			return false;
		}
		const float FontSize = ImGui::GetFontSize();
		const float DesiredPropertyWidth = ImGui::GetContentRegionAvail().x * Config.PropertyColumnWeight;
		const float PropertyWidth = std::clamp(
			DesiredPropertyWidth,
			FontSize * Config.MinimumPropertyColumnWidthInEm,
			FontSize * Config.MaximumPropertyColumnWidthInEm
		);
		ImGui::TableSetupColumn(Config.PropertyColumnLabel, ImGuiTableColumnFlags_WidthFixed, PropertyWidth);
		ImGui::TableSetupColumn(Config.ValueColumnLabel, ImGuiTableColumnFlags_WidthStretch, Config.ValueColumnWeight);
		if (Config.bShowHeaders) ImGui::TableHeadersRow();
		return true;
	}

	auto EndPropertyTable() -> void
	{
		ImGui::EndTable();
		ImGui::PopStyleVar();
	}

	auto BeginPropertyRow(const char* Label, bool bReadOnly, float LabelIndent) -> void
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		// Plain rows share the tree label column rather than starting under its disclosure arrow.
		const float EffectiveLabelIndent = LabelIndent > 0.0f ? LabelIndent : GetCompactTreeNodeToLabelSpacing();
		ImGui::Indent(EffectiveLabelIndent);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Label);
		ImGui::Unindent(EffectiveLabelIndent);
		if (bReadOnly && ImGui::IsItemHovered()) ImGui::SetTooltip("Read-only property");
		ImGui::TableSetColumnIndex(1);
		if (bReadOnly) ImGui::BeginDisabled();
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	auto EndPropertyRow(bool bReadOnly) -> void
	{
		if (bReadOnly) ImGui::EndDisabled();
	}

	auto EditTransformProperty(const char* Label, FTransform& Transform, bool bReadOnly) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = CompactTreeNode(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding);
		if (!bOpen) return false;

		auto EditTransformRow = [&](const char* RowLabel, FVector3& Value, double Speed) -> bool {
			BeginPropertyRow(RowLabel, bReadOnly, GetCompactTreeNodeToLabelSpacing());
			const bool bChanged = EditAxisValues(RowLabel, Value, Speed);
			EndPropertyRow(bReadOnly);
			return bChanged;
		};

		bool bChanged = EditTransformRow("Location", Transform.Translation, 0.05);
		FVector3 RotationDegrees = glm::degrees(glm::eulerAngles(Transform.Rotation));
		if (EditTransformRow("Rotation", RotationDegrees, 0.25))
		{
			Transform.Rotation = glm::quat(glm::radians(RotationDegrees));
			bChanged = true;
		}
		bChanged |= EditTransformRow("Scale", Transform.Scale3D, 0.01);
		ImGui::TreePop();
		return bChanged;
	}

	auto EditColorProperty(const char* Label, FLinearColor& Color, bool bShowAlpha, bool bReadOnly) -> bool
	{
		BeginPropertyRow(Label, bReadOnly);
		FLinearColor DisplayColor(LinearToSRGB(Color.R), LinearToSRGB(Color.G), LinearToSRGB(Color.B), std::clamp(Color.A, 0.0f, 1.0f));
		const ImGuiColorEditFlags Flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB;
		const bool bChanged = bShowAlpha
			? ImGui::ColorEdit4("##Value", DisplayColor.RGBA, Flags)
			: ImGui::ColorEdit3("##Value", DisplayColor.RGBA, Flags);
		if (bChanged)
		{
			Color.R = SRGBToLinear(DisplayColor.R);
			Color.G = SRGBToLinear(DisplayColor.G);
			Color.B = SRGBToLinear(DisplayColor.B);
			if (bShowAlpha) Color.A = DisplayColor.A;
		}
		EndPropertyRow(bReadOnly);
		return bChanged;
	}
} // namespace Durin::MonaImGui
