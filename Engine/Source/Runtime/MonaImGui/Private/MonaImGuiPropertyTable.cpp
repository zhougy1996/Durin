#include "MonaImGuiPropertyTable.h"

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
		const float EffectiveLabelIndent = LabelIndent > 0.0f ? LabelIndent : ImGui::GetStyle().FramePadding.x;
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
		const bool bOpen = ImGui::TreeNodeEx(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding);
		if (!bOpen) return false;

		auto EditTransformRow = [&](const char* RowLabel, FVector3& Value, double Speed) -> bool {
			BeginPropertyRow(RowLabel, bReadOnly, ImGui::GetTreeNodeToLabelSpacing());
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
} // namespace Durin::MonaImGui
