#include "PropertyEditor/DetailLayoutBuilder.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "DObject/Class.h"

#include "DObject/Property.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin::Editor
{
	FDetailLayoutBuilder::FDetailLayoutBuilder(std::string_view InSearchText, IDetailHostContext* InHostContext)
		: SearchText(InSearchText), HostContext(InHostContext)
	{
	}

	auto FDetailLayoutBuilder::ApplyCustomization(DObject* Object, IDetailCustomization& Customization) -> void
	{
		requiref(!bCustomizationBound || InspectedObject == Object, "A details layout cannot change its inspected object.");
		bCustomizationBound = true;
		InspectedObject = Object;
		Customization.CustomizeDetails(*this);
	}

	auto FDetailLayoutBuilder::ApplyRegisteredCustomizations(DObject* Object) -> void
	{
		requiref(!bCustomizationBound || InspectedObject == Object, "A details layout cannot change its inspected object.");
		bCustomizationBound = true;
		InspectedObject = Object;
		if (!Object) return;
		for (const auto& Customization : FDetailCustomizationRegistry::Get().FindDetailCustomizations(Object->GetClass()))
			ApplyCustomization(Object, *Customization);
	}


	auto FDetailLayoutBuilder::FindCategory(std::string_view Name) const -> const FCategory*
	{
		for (const auto& Category : Categories) if (Category->Name == Name) return Category.get();
		return nullptr;
	}

	auto FDetailLayoutBuilder::EditCategory(std::string_view Name, std::string_view DisplayName) -> IDetailCategoryBuilder&
	{
		for (const auto& Category : Categories)
		{
			if (Category->Name != Name) continue;
			if (!DisplayName.empty()) Category->Label = DisplayName;
			return *Category;
		}
		Categories.push_back(std::make_unique<FCategory>(*this, Name, DisplayName.empty() ? Name : DisplayName));
		return *Categories.back();
	}

	auto FDetailLayoutBuilder::HideCategory(std::string_view Name) -> void
	{
		HiddenCategories.emplace(Name);
	}

	auto FDetailLayoutBuilder::IsCategoryHidden(std::string_view Name) const -> bool
	{
		return HiddenCategories.contains(std::string(Name));
	}

	auto FDetailLayoutBuilder::FCategory::AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
		const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		const size_t Before = Owner.Rows.size();
		Owner.AddProperty(Object, Property, ArrayIndex, Options, SearchKeywords);
		if (Owner.Rows.size() != Before)
		{
			Owner.Rows.back().Category = Name;
		}
	}

	auto FDetailLayoutBuilder::FCategory::AddCustomRow(std::string_view SearchKeywords, FDetailRowDrawer Drawer) -> void
	{
		const size_t Before = Owner.Rows.size();
		Owner.AddCustomRow(SearchKeywords, std::move(Drawer));
		if (Owner.Rows.size() != Before) Owner.Rows.back().Category = Name;
	}

	auto FDetailLayoutBuilder::AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
		const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return;
		const std::string Label = Options.Label.empty() ? ::Durin::Editor::MakePropertyLabel(*Property, ArrayIndex) : Options.Label;
		auto RowOptions = Options;
		RowOptions.Label = Label;
		Rows.push_back({Object, Property, ArrayIndex, std::move(RowOptions),
			std::format("{} {} {}", Property->NamePrivate, Label, SearchKeywords), {}});
	}

	auto FDetailLayoutBuilder::AddCustomRow(std::string_view SearchKeywords, FCustomRowDrawer Drawer) -> void
	{
		if (!Drawer) return;
		Rows.push_back({nullptr, nullptr, 0, {}, std::string(SearchKeywords), std::move(Drawer)});
	}

	auto FDetailLayoutBuilder::HideProperty(FProperty* Property) -> void
	{
		if (Property) HiddenProperties.insert(Property);
	}

	auto FDetailLayoutBuilder::ReplaceDefaultProperties() -> void
	{
		bReplaceDefaultProperties = true;
	}


	auto FDetailLayoutBuilder::IsPropertyHidden(const FProperty& Property) const -> bool
	{
		return HiddenProperties.contains(const_cast<FProperty*>(&Property));
	}

	auto FDetailLayoutBuilder::IsDefaultPropertyPlaced(const DObject* Object, const FProperty& Property, uint32 ArrayIndex) const -> bool
	{
		return std::ranges::any_of(Rows, [&](const FRow& Row) {
			return Row.Object == Object && Row.Property == &Property && Row.ArrayIndex == ArrayIndex;
		});
	}

	auto FDetailLayoutBuilder::ShouldDrawDefaultProperty(const DObject* Object, const FProperty& Property, uint32 ArrayIndex) const -> bool
	{
		return !bReplaceDefaultProperties && !IsPropertyHidden(Property)
			&& !IsCategoryHidden(Property.GetTypedMetadata().Category)
			&& !IsDefaultPropertyPlaced(Object, Property, ArrayIndex);
	}

	auto FDetailLayoutBuilder::IsReplacingDefaultProperties() const -> bool
	{
		return bReplaceDefaultProperties;
	}

	auto FDetailLayoutBuilder::MatchesSearch(std::string_view Candidate) const -> bool
	{
		return SearchText.empty() || StringUtils::ContainsInsensitive(Candidate, SearchText);
	}

	auto FDetailLayoutBuilder::GetVisibleRowCount() const -> uint32
	{
		return static_cast<uint32>(std::ranges::count_if(Rows, [this](const FRow& Row) { return IsRowVisible(Row); }));
	}

	auto FDetailLayoutBuilder::IsRowVisible(const FRow& Row) const -> bool
	{
		if (IsCategoryHidden(Row.Category)) return false;
		const FCategory* Category = FindCategory(Row.Category);
		return MatchesSearch(Row.SearchText) || (Category && (MatchesSearch(Category->Name) || MatchesSearch(Category->Label)));
	}

	auto FDetailLayoutBuilder::DrawRows(::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& ViewContext) const -> FDetailLayoutBuilderResult
	{
		FDetailLayoutBuilderResult Result;
		std::vector<const FRow*> VisibleRows;
		for (const FRow& Row : Rows) if (IsRowVisible(Row)) VisibleRows.push_back(&Row);
		std::stable_sort(VisibleRows.begin(), VisibleRows.end(), [this](const FRow* Left, const FRow* Right) {
			const FCategory* LeftCategory = FindCategory(Left->Category);
			const FCategory* RightCategory = FindCategory(Right->Category);
			const int32 LeftOrder = LeftCategory ? LeftCategory->SortOrder : 0;
			const int32 RightOrder = RightCategory ? RightCategory->SortOrder : 0;
			return LeftOrder != RightOrder ? LeftOrder < RightOrder : Left->Category < Right->Category;
		});
		std::string CurrentCategory;
		bool bGroupOpen = true;
		for (const FRow* RowPtr : VisibleRows)
		{
			const FRow& Row = *RowPtr;
			if (Row.Category != CurrentCategory)
			{
				if (!CurrentCategory.empty() && bGroupOpen) MonaImGui::PropertyEdit::EndGroup();
				CurrentCategory = Row.Category;
				bGroupOpen = true;
				if (!CurrentCategory.empty())
				{
					if (!SearchText.empty()) ImGui::GetStateStorage()->SetInt(ImGui::GetID(CurrentCategory.c_str()), 1);
					const FCategory* Category = FindCategory(CurrentCategory);
					bGroupOpen = MonaImGui::PropertyEdit::BeginGroup(CurrentCategory.c_str(), Category->Label.c_str());
					if (!Category->Tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s", Category->Tooltip.c_str());
				}
			}
			++Result.VisibleRowCount;
			if (!bGroupOpen) continue;
			if (Row.Property)
				Result.bChanged |= PropertyView.EditProperty(ViewContext, Row.Object, Row.Property, Row.ArrayIndex, Row.Options);
			else
				Result.bChanged |= Row.Drawer(PropertyView, ViewContext);
		}
		if (!CurrentCategory.empty() && bGroupOpen) MonaImGui::PropertyEdit::EndGroup();
		return Result;
	}

}
