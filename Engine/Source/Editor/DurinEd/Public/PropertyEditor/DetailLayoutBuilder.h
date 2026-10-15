#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "PropertyEditor/DetailCustomization.h"
#include "PropertyEditor/EditorFunctionAction.h"

namespace Durin::Editor
{
	// Summarizes rows and changes produced by an object details builder.
	struct FDetailLayoutBuilderResult
	{
		uint32 VisibleRowCount = 0;
		bool bChanged = false;
		std::optional<FEditorFunctionRequest> FunctionRequest;
	};

	// Builds searchable reflected and custom rows for one details view.
	class FDetailLayoutBuilder : public IDetailLayoutBuilder
	{
	public:
		using FCustomRowDrawer = FDetailRowDrawer;

		DURINED_API explicit FDetailLayoutBuilder(std::string_view InSearchText = {}, IDetailHostContext* InHostContext = nullptr);
		FDetailLayoutBuilder(const FDetailLayoutBuilder&) = delete;
		FDetailLayoutBuilder(FDetailLayoutBuilder&&) = delete;
		auto operator=(const FDetailLayoutBuilder&) -> FDetailLayoutBuilder& = delete;
		auto operator=(FDetailLayoutBuilder&&) -> FDetailLayoutBuilder& = delete;
		DURINED_API auto ApplyCustomization(DObject* Object, IDetailCustomization& Customization) -> void;
		DURINED_API auto ApplyRegisteredCustomizations(DObject* Object) -> void;
		auto GetObject() const -> DObject* override { return InspectedObject; }
		auto GetSearchText() const -> std::string_view override { return SearchText; }
		auto GetHostContext() const -> IDetailHostContext* override { return HostContext; }
		DURINED_API auto EditCategory(std::string_view Name, std::string_view DisplayName = {}) -> IDetailCategoryBuilder& override;
		DURINED_API auto HideCategory(std::string_view Name) -> void override;
		DURINED_API auto IsCategoryHidden(std::string_view Name) const -> bool;
		DURINED_API auto AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex = 0,
			const ::Durin::Editor::FPropertyViewOptions& Options = {}, std::string_view SearchKeywords = {}) -> void override;
		DURINED_API auto AddCustomRow(std::string_view SearchKeywords, FCustomRowDrawer Drawer) -> void override;
		DURINED_API auto HideProperty(FProperty* Property) -> void override;
		DURINED_API auto ReplaceDefaultProperties() -> void override;
		DURINED_API auto IsPropertyHidden(const FProperty& Property) const -> bool;
		DURINED_API auto IsDefaultPropertyPlaced(const DObject* Object, const FProperty& Property, uint32 ArrayIndex) const -> bool;
		DURINED_API auto ShouldDrawDefaultProperty(const DObject* Object, const FProperty& Property, uint32 ArrayIndex) const -> bool;
		DURINED_API auto IsReplacingDefaultProperties() const -> bool;
		// Appends remaining reflected rows to the same category layout as custom rows.
		DURINED_API auto AddDefaultProperties(DObject* Object, bool bIncludeTransient = true) -> void;
		DURINED_API auto AddDefaultFunctions() -> void;
		DURINED_API auto AddFunction(const FFunction& Function) -> void override;
		DURINED_API auto HideFunction(const FFunction& Function) -> void override;
		DURINED_API auto GetVisibleRowCount() const -> uint32;
		DURINED_API auto DrawRows(::Durin::Editor::FPropertyView& PropertyView,
			const ::Durin::Editor::FPropertyViewContext& ViewContext) const -> FDetailLayoutBuilderResult;

	private:
		class FCategory;
		struct FPropertyRowKey
		{
			const DObject* Object;
			const FProperty* Property;
			uint32 ArrayIndex;
			auto operator==(const FPropertyRowKey&) const -> bool = default;
		};
		struct FPropertyRowKeyHash
		{
			auto operator()(const FPropertyRowKey& Key) const -> size_t
			{
				return std::hash<const DObject*>{}(Key.Object) ^ (std::hash<const FProperty*>{}(Key.Property) << 1)
					^ (std::hash<uint32>{}(Key.ArrayIndex) << 2);
			}
		};

		// Stores either one reflected-property row or one custom drawer.
		struct FRow
		{
			DObject* Object = nullptr;
			FProperty* Property = nullptr;
			uint32 ArrayIndex = 0;
			FPropertyViewOptions Options;
			std::string SearchText;
			FCustomRowDrawer Drawer;
			const FCategory* Category = nullptr;
			const FFunction* Function = nullptr;
		};

		// Stores category presentation and forwards its rows to the host layout.
		class FCategory final : public IDetailCategoryBuilder
		{
		public:
			FCategory(FDetailLayoutBuilder& InOwner, std::string_view InName, std::string_view InLabel)
				: Owner(InOwner), Name(InName), Label(InLabel) {}
			auto AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex,
				const ::Durin::Editor::FPropertyViewOptions& Options, std::string_view SearchKeywords) -> void override;
			auto AddCustomRow(std::string_view SearchKeywords, FDetailRowDrawer Drawer) -> void override;
			auto AddFunction(const FFunction& Function) -> void override;
			auto SetSortOrder(int32 InOrder) -> void override { SortOrder = InOrder; Owner.bVisibleRowsDirty = true; }
			auto SetTooltip(std::string_view InTooltip) -> void override { Tooltip = InTooltip; }
			auto SetInitiallyCollapsed(bool bCollapsed) -> void override { bInitiallyCollapsed = bCollapsed; }
			FDetailLayoutBuilder& Owner;
			std::string Name;
			std::string Label;
			std::string Tooltip;
			int32 SortOrder = 0;
			bool bInitiallyCollapsed = false;
		};

		auto MatchesSearch(std::string_view Candidate) const -> bool;
		auto IsRowVisible(const FRow& Row) const -> bool;
		auto PrepareVisibleRows() const -> void;

		std::string SearchText;
		std::vector<FRow> Rows;
		std::unordered_set<FPropertyRowKey, FPropertyRowKeyHash> PlacedProperties;
		mutable std::vector<const FRow*> VisibleRows;
		mutable bool bVisibleRowsDirty = true;
		std::unordered_set<FProperty*> HiddenProperties;
		std::unordered_set<const FFunction*> PlacedFunctions;
		std::unordered_set<const FFunction*> HiddenFunctions;
		bool bReplaceDefaultProperties = false;
		std::vector<std::unique_ptr<FCategory>> Categories;
		std::unordered_set<std::string> HiddenCategories;
		DObject* InspectedObject = nullptr;
		IDetailHostContext* HostContext = nullptr;
		bool bCustomizationBound = false;
	};

}
