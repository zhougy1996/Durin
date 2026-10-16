#pragma once

#include "CoreMinimal.h"
#include "PropertyEditor/PropertyView.h"
#include "DObject/Function.h"

namespace Durin::Editor
{
	// Supplies optional host-specific services without a dependency on a feature editor.
	class IDetailHostContext
	{
	public:
		virtual ~IDetailHostContext() = default;
	};
	using FDetailRowDrawer = std::function<bool(::Durin::Editor::FPropertyView&, const ::Durin::Editor::FPropertyViewContext&)>;

	// Declares the contents and order of a named details category.
	class IDetailCategoryBuilder
	{
	public:
		virtual ~IDetailCategoryBuilder() = default;
		virtual auto AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex = 0,
			const ::Durin::Editor::FPropertyViewOptions& Options = {}, std::string_view SearchKeywords = {}) -> void = 0;
		virtual auto AddCustomRow(std::string_view SearchKeywords, FDetailRowDrawer Drawer) -> void = 0;
		virtual auto AddFunction(const DFunction& Function) -> void = 0;
		virtual auto SetSortOrder(int32 SortOrder) -> void = 0;
		virtual auto SetTooltip(std::string_view Tooltip) -> void = 0;
		virtual auto SetInitiallyCollapsed(bool bCollapsed) -> void = 0;
	};

	// Supplies the inspected object, host services, and declarative layout operations.
	class IDetailLayoutBuilder
	{
	public:
		virtual ~IDetailLayoutBuilder() = default;
		virtual auto GetObject() const -> DObject* = 0;
		virtual auto GetSearchText() const -> std::string_view = 0;
		virtual auto GetHostContext() const -> IDetailHostContext* = 0;
		virtual auto EditCategory(std::string_view Name, std::string_view DisplayName = {}) -> IDetailCategoryBuilder& = 0;
		virtual auto HideCategory(std::string_view Name) -> void = 0;
		virtual auto AddProperty(DObject* Object, FProperty* Property, uint32 ArrayIndex = 0,
			const ::Durin::Editor::FPropertyViewOptions& Options = {}, std::string_view SearchKeywords = {}) -> void = 0;
		virtual auto AddCustomRow(std::string_view SearchKeywords, FDetailRowDrawer Drawer) -> void = 0;
		virtual auto HideProperty(FProperty* Property) -> void = 0;
		virtual auto ReplaceDefaultProperties() -> void = 0;
		virtual auto AddFunction(const DFunction& Function) -> void = 0;
		virtual auto HideFunction(const DFunction& Function) -> void = 0;
	};

	// Defines an editor-side layout for one reflected object class.
	class IDetailCustomization
	{
	public:
		virtual ~IDetailCustomization() = default;
		virtual auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void = 0;
	};
}
