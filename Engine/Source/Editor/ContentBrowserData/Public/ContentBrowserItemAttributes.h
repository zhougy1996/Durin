#pragma once
#include "CoreMinimal.h"

namespace Durin::ContentBrowser
{
	struct FContentBrowserItemDataAttributeMetaData
	{
		std::string DisplayName, TooltipText, Suffix;
		bool bIsImportant = false;
	};
	// Scalars retain size/time precision without presentation-layer string parsing.
	class FContentBrowserItemDataAttributeValue
	{
	  public:
		using FValue = std::variant<std::monostate, std::string, bool, int64, uint64, double>;
		FContentBrowserItemDataAttributeValue() = default;
		explicit FContentBrowserItemDataAttributeValue(std::string Value) : Value(std::move(Value)) {}
		explicit FContentBrowserItemDataAttributeValue(std::string_view Value) : Value(std::string(Value)) {}
		explicit FContentBrowserItemDataAttributeValue(const char* Text) { if (Text) Value=std::string(Text); }
		explicit FContentBrowserItemDataAttributeValue(bool InValue) : Value(InValue) {}
		template<typename T> requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
		explicit FContentBrowserItemDataAttributeValue(T InValue)
			: Value(static_cast<std::conditional_t<std::is_signed_v<T>, int64, uint64>>(InValue)) {}
		explicit FContentBrowserItemDataAttributeValue(double Value) : Value(Value) {}
		auto IsValid() const -> bool { return !std::holds_alternative<std::monostate>(Value); }
		template <typename T> auto GetValue() const -> const T* { return std::get_if<T>(&Value); }
		auto GetMetaData() const -> const std::optional<FContentBrowserItemDataAttributeMetaData>& { return MetaData; }
		auto SetMetaData(FContentBrowserItemDataAttributeMetaData InMetaData) -> void { MetaData = std::move(InMetaData); }

	  private:
		FValue Value;
		std::optional<FContentBrowserItemDataAttributeMetaData> MetaData;
	};
	using FContentBrowserItemDataAttributeValues = std::unordered_map<std::string, FContentBrowserItemDataAttributeValue>;
	namespace ContentBrowserItemAttributes
	{
		inline constexpr std::string_view ItemTypeName = "ItemTypeName";
		inline constexpr std::string_view ItemTypeDisplayName = "ItemTypeDisplayName";
		inline constexpr std::string_view Class = "Class";
		inline constexpr std::string_view ItemDiskSize = "ItemDiskSize";
		inline constexpr std::string_view AssetPath = "AssetPath";
		inline constexpr std::string_view PackagePath = "PackagePath";
		inline constexpr std::string_view Extension = "Extension";
		inline constexpr std::string_view IsRedirector = "IsRedirector";
		inline constexpr std::string_view RedirectDestination = "RedirectDestination";
		inline constexpr std::string_view PackageFileSize = "PackageFileSize";
		inline constexpr std::string_view BulkFileSize = "BulkFileSize";
		inline constexpr std::string_view LastWriteTime = "LastWriteTime";
	} // namespace ContentBrowserItemAttributes
} // namespace Durin::ContentBrowser
