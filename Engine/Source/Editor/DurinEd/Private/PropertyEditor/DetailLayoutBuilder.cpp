#include "PropertyEditor/DetailLayoutBuilder.h"
#include "Profiling/Profiling.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "DObject/Class.h"

#include "DObject/Property.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
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


	auto FDetailLayoutBuilder::EditCategory(std::string_view Name, std::string_view DisplayName) -> IDetailCategoryBuilder&
	{
		for (const auto& Category : Categories)
		{
			if (Category->Name != Name) continue;
			if (!DisplayName.empty())
			{
				Category->Label = DisplayName;
				bVisibleRowsDirty = true;
			}
			return *Category;
		}
		Categories.push_back(std::make_unique<FCategory>(*this, Name, DisplayName.empty() ? Name : DisplayName));
		if (Name == "Transform") Categories.back()->SortOrder = -100;
		if (Name == "Diagnostics")
		{
			Categories.back()->SortOrder = 100;
			Categories.back()->bInitiallyCollapsed = true;
		}
		return *Categories.back();
	}

	auto FDetailLayoutBuilder::HideCategory(std::string_view Name) -> void
	{
		HiddenCategories.emplace(Name);
		bVisibleRowsDirty = true;
	}

	auto FDetailLayoutBuilder::IsCategoryHidden(std::string_view Name) const -> bool
	{
		return !HiddenCategories.empty() && HiddenCategories.contains(std::string(Name));
	}

	auto FDetailLayoutBuilder::FCategory::AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
		const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		const size_t Before = Owner.Rows.size();
		Owner.AddProperty(Object, Property, ArrayIndex, Options, SearchKeywords);
		if (Owner.Rows.size() != Before)
		{
			Owner.Rows.back().Category = this;
		}
	}

	auto FDetailLayoutBuilder::FCategory::AddCustomRow(std::string_view SearchKeywords, FDetailRowDrawer Drawer) -> void
	{
		const size_t Before = Owner.Rows.size();
		Owner.AddCustomRow(SearchKeywords, std::move(Drawer));
		if (Owner.Rows.size() != Before) Owner.Rows.back().Category = this;
	}

	auto FDetailLayoutBuilder::AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
		const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void
	{
		if (!Object || !Property || ArrayIndex >= Property->GetArrayDim()) return;
		const std::string Label = Options.Label.empty() ? ::Durin::Editor::MakePropertyLabel(*Property, ArrayIndex) : Options.Label;
		auto RowOptions = Options;
		RowOptions.Label = Label;
		std::string RowSearchText;
		if (!SearchText.empty())
		{
			const std::string SourceName = Property->GetArrayDim() > 1
				? std::format("{}[{}]", Property->NamePrivate, ArrayIndex) : Property->NamePrivate.ToString();
			const bool bTransform = Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
				&& static_cast<const FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_FTransform();
			RowSearchText = std::format("{} {} {} {} {} {}", SourceName, Label, SearchKeywords,
				Property->GetTypedMetadata().Category, Property->GetTypedMetadata().ToolTip,
				bTransform ? "Location Rotation Scale" : "");
		}
		Rows.push_back({Object, Property, ArrayIndex, std::move(RowOptions), std::move(RowSearchText), {}});
		PlacedProperties.insert({Object, Property, ArrayIndex});
		bVisibleRowsDirty = true;
	}

	auto FDetailLayoutBuilder::AddCustomRow(std::string_view SearchKeywords, FCustomRowDrawer Drawer) -> void
	{
		if (!Drawer) return;
		Rows.push_back({nullptr, nullptr, 0, {}, SearchText.empty() ? std::string{} : std::string(SearchKeywords), std::move(Drawer)});
		bVisibleRowsDirty = true;
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
		return PlacedProperties.contains({Object, &Property, ArrayIndex});
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

	auto FDetailLayoutBuilder::AddDefaultProperties(DObject* Object, bool bIncludeTransient) -> void
	{
		if (!Object || bReplaceDefaultProperties) return;
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (!Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit)
				|| (!bIncludeTransient && Property->HasAnyPropertyFlags(EPropertyFlags::Transient))) return;
			const auto& Category = Property->GetTypedMetadata().Category;
			const auto EnumName = Property->GetMetaData(FName("ArrayIndexEnum"));
			const DEnum* IndexEnum = Property->GetArrayDim() > 1 && !EnumName.empty()
				? FindEnumByQualifiedName(FName(EnumName)) : nullptr;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				if (IndexEnum && !IndexEnum->FindValueRecordByValue(Index)) continue;
				if (!ShouldDrawDefaultProperty(Object, *Property, Index)) continue;
				if (Category.empty()) AddProperty(Object, Property, Index);
				else EditCategory(Category).AddProperty(Object, Property, Index,
					{.bShowTransformHeader = Category != "Transform"});
			}
		});
	}

	auto FDetailLayoutBuilder::GetVisibleRowCount() const -> uint32
	{
		PrepareVisibleRows();
		return static_cast<uint32>(VisibleRows.size());
	}

	auto FDetailLayoutBuilder::IsRowVisible(const FRow& Row) const -> bool
	{
		const FCategory* Category = Row.Category;
		if (IsCategoryHidden(Category ? std::string_view(Category->Name) : std::string_view{})) return false;
		return MatchesSearch(Row.SearchText) || (Category && (MatchesSearch(Category->Name) || MatchesSearch(Category->Label)));
	}

	auto FDetailLayoutBuilder::PrepareVisibleRows() const -> void
	{
		if (!bVisibleRowsDirty) return;
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.FilterAndSortRows");
		VisibleRows.clear();
		VisibleRows.reserve(Rows.size());
		for (const FRow& Row : Rows) if (IsRowVisible(Row)) VisibleRows.push_back(&Row);
		std::stable_sort(VisibleRows.begin(), VisibleRows.end(), [](const FRow* Left, const FRow* Right) {
			const int32 LeftOrder = Left->Category ? Left->Category->SortOrder : 0;
			const int32 RightOrder = Right->Category ? Right->Category->SortOrder : 0;
			const std::string_view LeftName = Left->Category ? std::string_view(Left->Category->Name) : std::string_view{};
			const std::string_view RightName = Right->Category ? std::string_view(Right->Category->Name) : std::string_view{};
			return LeftOrder != RightOrder ? LeftOrder < RightOrder : LeftName < RightName;
		});
		bVisibleRowsDirty = false;
	}

	auto FDetailLayoutBuilder::DrawRows(::Durin::Editor::FPropertyView& PropertyView,
		const ::Durin::Editor::FPropertyViewContext& ViewContext) const -> FDetailLayoutBuilderResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Details.DrawRows");
		FDetailLayoutBuilderResult Result;
		PrepareVisibleRows();
		const FCategory* CurrentCategory = nullptr;
		bool bGroupOpen = true;
		for (const FRow* RowPtr : VisibleRows)
		{
			const FRow& Row = *RowPtr;
			if (Row.Category != CurrentCategory)
			{
				if (CurrentCategory != nullptr && bGroupOpen) MonaImGui::PropertyEdit::EndGroup();
				CurrentCategory = Row.Category;
				bGroupOpen = true;
				if (CurrentCategory != nullptr)
				{
					if (!SearchText.empty()) ImGui::GetStateStorage()->SetInt(ImGui::GetID(CurrentCategory->Name.c_str()), 1);
					const FCategory* Category = CurrentCategory;
					bGroupOpen = MonaImGui::PropertyEdit::BeginGroup(CurrentCategory->Name.c_str(), Category->Label.c_str(),
						Category->bInitiallyCollapsed ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen);
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
		if (CurrentCategory != nullptr && bGroupOpen) MonaImGui::PropertyEdit::EndGroup();
		return Result;
	}

}
