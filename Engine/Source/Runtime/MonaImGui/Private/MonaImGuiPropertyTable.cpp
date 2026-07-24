#include "MonaImGuiPropertyTable.h"

#include "Math/Color.h"
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
		auto EditComponentValues(const char* Id, TVector& Value, double Speed, FWidgetState* State) -> bool
		{
			static_assert(NumComponents >= 2 && NumComponents <= 4);
			static constexpr std::array<const char*, 4> ComponentNames = {"X", "Y", "Z", "W"};
			const std::array<ImVec4, 4> ComponentColors = {
				GetThemeColor(EUIThemeColor::AxisX),
				GetThemeColor(EUIThemeColor::AxisY),
				GetThemeColor(EUIThemeColor::AxisZ),
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
			};

			ImGui::PushID(Id);
			bool bChanged = false;
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ScaleUI(3.0f), 0.0f));
			if (ImGui::BeginTable("##Components", static_cast<int>(NumComponents), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
			{
				for (size_t Component = 0; Component < NumComponents; ++Component)
				{
					ImGui::TableNextColumn();
					ImGui::AlignTextToFramePadding();
					ImGui::TextColored(ComponentColors[Component], "%s", ComponentNames[Component]);
					ImGui::SameLine(0.0f, ScaleUI(3.0f));
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushID(static_cast<int>(Component));
					bChanged |= ImGui::DragScalar("##Value", ImGuiDataType_Double, &Value[Component], static_cast<float>(Speed));
					AccumulateLastItemState(State);
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

		auto QuatToEulerDegrees(const FQuat& Value) -> FVector3
		{
			// Invalid quaternion storage should remain recoverable from the editor instead of producing NaN controls.
			const FQuat Normalized = glm::dot(Value, Value) > 1.e-12 ? glm::normalize(Value) : glm::identity<FQuat>();
			return glm::degrees(glm::eulerAngles(Normalized));
		}

		auto EulerDegreesToQuat(const FVector3& Value) -> FQuat
		{
			return glm::normalize(glm::quat(glm::radians(Value)));
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

	auto EditVector(const char* Label, FVector2& Value, bool bReadOnly, double Speed, FWidgetState* OutState) -> bool
	{
		BeginRow(Label, bReadOnly);
		const bool bChanged = EditComponentValues<2>(Label, Value, Speed, OutState);
		EndRow(bReadOnly);
		return bChanged;
	}

	auto EditVector(const char* Label, FVector3& Value, bool bReadOnly, double Speed, FWidgetState* OutState) -> bool
	{
		BeginRow(Label, bReadOnly);
		const bool bChanged = EditComponentValues<3>(Label, Value, Speed, OutState);
		EndRow(bReadOnly);
		return bChanged;
	}

	auto EditVector(const char* Label, FVector4& Value, bool bReadOnly, double Speed, FWidgetState* OutState) -> bool
	{
		BeginRow(Label, bReadOnly);
		const bool bChanged = EditComponentValues<4>(Label, Value, Speed, OutState);
		EndRow(bReadOnly);
		return bChanged;
	}

	auto EditQuat(const char* Label, FQuat& Value, bool bReadOnly, FWidgetState* OutState) -> bool
	{
		FVector3 RotationDegrees = QuatToEulerDegrees(Value);
		if (!EditVector(Label, RotationDegrees, bReadOnly, 0.25, OutState)) return false;
		Value = EulerDegreesToQuat(RotationDegrees);
		return true;
	}

	auto EndTable() -> void
	{
		ImGui::EndTable();
		ImGui::PopStyleVar();
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

	auto BeginRow(const char* Label, bool bReadOnly, float LabelIndent) -> void
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

	auto EndRow(bool bReadOnly) -> void
	{
		if (bReadOnly) ImGui::EndDisabled();
	}

	auto EditTransform(const char* Label, FTransform& Transform, bool bReadOnly, FWidgetState* OutState) -> bool
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const bool bOpen = CompactTreeNode(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns | ImGuiTreeNodeFlags_FramePadding);
		if (!bOpen) return false;

		auto EditTransformRow = [&](const char* RowLabel, FVector3& Value, double Speed) -> bool {
			BeginRow(RowLabel, bReadOnly, GetCompactTreeNodeToLabelSpacing());
			const bool bChanged = EditComponentValues<3>(RowLabel, Value, Speed, OutState);
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

	auto EditColor(const char* Label, FLinearColor& Color, bool bShowAlpha, bool bReadOnly, FWidgetState* OutState) -> bool
	{
		ImGui::PushID(Label);
		BeginRow(Label, bReadOnly);
		FLinearColor DisplayColor(LinearToSRGB(Color.R), LinearToSRGB(Color.G), LinearToSRGB(Color.B), std::clamp(Color.A, 0.0f, 1.0f));
		const ImGuiColorEditFlags Flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB;
		const bool bChanged = bShowAlpha
			? ImGui::ColorEdit4("##Value", DisplayColor.RGBA, Flags)
			: ImGui::ColorEdit3("##Value", DisplayColor.RGBA, Flags);
		AccumulateLastItemState(OutState);
		if (bChanged)
		{
			Color.R = SRGBToLinear(DisplayColor.R);
			Color.G = SRGBToLinear(DisplayColor.G);
			Color.B = SRGBToLinear(DisplayColor.B);
			if (bShowAlpha) Color.A = DisplayColor.A;
		}
		EndRow(bReadOnly);
		ImGui::PopID();
		return bChanged;
	}
} // namespace Durin::MonaImGui::PropertyEdit
