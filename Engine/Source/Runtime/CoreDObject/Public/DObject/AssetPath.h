#pragma once

#include "CoreDObjectAPI.h"
#include "Misc/Name.h"

namespace Durin
{
	inline constexpr size_t MaximumObjectPathComponentBytes = 1024;
	inline constexpr size_t MaximumObjectPathBytes = 1024 * 1024;

	class FPackagePath
	{
	public:
		FPackagePath() = default;
		COREDOBJECT_API static auto TryCreate(std::string_view InPath, FPackagePath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreateProjectContent(std::string_view InPath, FPackagePath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto IsValid(std::string_view InPath, std::string* OutError = nullptr) -> bool;
		auto IsValid() const -> bool { return !Path.IsNone(); }
		COREDOBJECT_API auto ToString() const -> std::string;
		COREDOBJECT_API auto GetView() const -> std::string_view;
		auto GetPackageName() const -> std::string_view { const auto View = GetView(); const size_t Slash = View.find_last_of('/'); return Slash == std::string_view::npos ? View : View.substr(Slash + 1); }
		auto GetAssetName() const -> std::string_view { return GetPackageName(); }
		auto operator==(const FPackagePath&) const -> bool = default;
		COREDOBJECT_API auto operator<=>(const FPackagePath& Other) const -> std::strong_ordering;
	private:
		explicit FPackagePath(FName InPath) : Path(std::move(InPath)) {}
		FName Path;
		friend struct std::hash<FPackagePath>;
	};

	class FTopLevelAssetPath
	{
	public:
		FTopLevelAssetPath() = default;
		COREDOBJECT_API static auto TryCreate(std::string_view InPath, FTopLevelAssetPath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreate(const FPackagePath& InPackagePath, std::string_view InAssetName, FTopLevelAssetPath& OutPath, std::string* OutError = nullptr) -> bool;
		auto IsValid() const -> bool { return PackagePath.IsValid() && !AssetName.IsNone(); }
		auto GetPackagePath() const -> const FPackagePath& { return PackagePath; }
		COREDOBJECT_API auto GetAssetName() const -> std::string_view;
		COREDOBJECT_API auto AppendTo(std::string& Out) const -> void;
		COREDOBJECT_API auto ToString() const -> std::string;
		auto operator==(const FTopLevelAssetPath&) const -> bool = default;
		COREDOBJECT_API auto operator<=>(const FTopLevelAssetPath& Other) const -> std::strong_ordering;
	private:
		FPackagePath PackagePath;
		FName AssetName;
		friend struct std::hash<FTopLevelAssetPath>;
	};

	class FSubobjectPathView
	{
	public:
		class FIterator
		{
		public:
			using value_type = std::string_view; using difference_type = std::ptrdiff_t; using iterator_category = std::forward_iterator_tag;
			COREDOBJECT_API auto operator*() const -> std::string_view;
			COREDOBJECT_API auto operator++() -> FIterator&;
			auto operator++(int) -> FIterator { FIterator Copy = *this; ++*this; return Copy; }
			auto operator==(const FIterator&) const -> bool = default;
		private:
			FIterator(std::string_view InPath, size_t InOffset) : Path(InPath), Offset(InOffset) {}
			std::string_view Path; size_t Offset = 0; friend class FSubobjectPathView;
		};
		auto begin() const -> FIterator { return FIterator(Path, 0); }
		auto end() const -> FIterator { return FIterator(Path, Path.size()); }
		COREDOBJECT_API auto size() const -> size_t;
		COREDOBJECT_API auto operator[](size_t Index) const -> std::string_view;
		auto empty() const -> bool { return Path.empty(); }
	private:
		explicit FSubobjectPathView(std::string_view InPath) : Path(InPath) {}
		std::string_view Path; friend class FObjectPath;
	};

	class FObjectPath
	{
	public:
		FObjectPath() = default;
		COREDOBJECT_API static auto TryCreate(std::string_view InPath, FObjectPath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreate(const FTopLevelAssetPath& InAssetPath, std::span<const std::string> InSubobjectNames, FObjectPath& OutPath, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API static auto TryCreate(const FTopLevelAssetPath& InAssetPath, FSubobjectPathView InSubobjectNames, FObjectPath& OutPath, std::string* OutError = nullptr) -> bool;
		auto IsValid() const -> bool { return AssetPath.IsValid(); }
		auto GetAssetPath() const -> const FTopLevelAssetPath& { return AssetPath; }
		auto GetPackagePath() const -> const FPackagePath& { return AssetPath.GetPackagePath(); }
		auto GetSubobjectNames() const -> FSubobjectPathView { return FSubobjectPathView(SubobjectPath); }
		auto IsTopLevelAsset() const -> bool { return IsValid() && SubobjectPath.empty(); }
		COREDOBJECT_API auto AppendTo(std::string& Out) const -> void;
		COREDOBJECT_API auto ToString() const -> std::string;
		COREDOBJECT_API auto operator==(const FObjectPath& Other) const -> bool;
		COREDOBJECT_API auto operator<=>(const FObjectPath& Other) const -> std::strong_ordering;
	private:
		FTopLevelAssetPath AssetPath;
		std::string SubobjectPath;
		friend struct std::hash<FObjectPath>;
	};
	using FAssetPath = FPackagePath;
}

template<> struct std::hash<Durin::FPackagePath> { auto operator()(const Durin::FPackagePath& Value) const noexcept -> size_t { return std::hash<Durin::FName>{}(Value.Path); } };
template<> struct std::hash<Durin::FTopLevelAssetPath> { auto operator()(const Durin::FTopLevelAssetPath& Value) const noexcept -> size_t { size_t Hash = std::hash<Durin::FPackagePath>{}(Value.PackagePath); Hash ^= std::hash<Durin::FName>{}(Value.AssetName) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2); return Hash; } };
template<> struct std::hash<Durin::FObjectPath> { auto operator()(const Durin::FObjectPath& Value) const noexcept -> size_t { size_t SubobjectHash = 1469598103934665603ull; for (const char Character : Value.SubobjectPath) { const char Folded = Character >= 'A' && Character <= 'Z' ? Character + ('a' - 'A') : Character; SubobjectHash = (SubobjectHash ^ static_cast<uint8_t>(Folded)) * 1099511628211ull; } size_t Hash = std::hash<Durin::FTopLevelAssetPath>{}(Value.AssetPath); Hash ^= SubobjectHash + 0x9e3779b9 + (Hash << 6) + (Hash >> 2); return Hash; } };
