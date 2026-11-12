#pragma once
#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"
#include "ContentBrowserAPI.h"

namespace Durin::ContentBrowser
{
	class FContentBrowserItemData;
	struct FContentBrowserItem;
	enum class EContentBrowserItemFlags : uint16
	{
		None = 0,
		Type_Folder = 1 << 0,
		Type_File = 1 << 1,
		Type_MASK = Type_Folder | Type_File,
		Category_Asset = 1 << 2,
		Category_Class = 1 << 3,
		Category_Collection = 1 << 4,
		Category_Plugin = 1 << 5,
		Category_Misc = 1 << 6,
		Category_MASK = Category_Asset | Category_Class | Category_Collection | Category_Plugin | Category_Misc,
		Temporary_Creation = 1 << 7,
		Temporary_Duplication = 1 << 8,
		Temporary_MASK = Temporary_Creation | Temporary_Duplication,
		Misc_Unsupported = 1 << 9
	};
	constexpr auto operator|(EContentBrowserItemFlags A, EContentBrowserItemFlags B) -> EContentBrowserItemFlags
	{
		return static_cast<EContentBrowserItemFlags>(static_cast<uint16>(A) | static_cast<uint16>(B));
	}
	// UE's source-local key: category and temporary flags do not define identity.
	class FContentBrowserItemDataKey
	{
	public:
		FContentBrowserItemDataKey() = default;
		CONTENTBROWSER_API explicit FContentBrowserItemDataKey(const FContentBrowserItemData&);
		explicit FContentBrowserItemDataKey(EContentBrowserItemFlags Flags, std::string_view Path)
			: Type(static_cast<EContentBrowserItemFlags>(static_cast<uint16>(Flags) & static_cast<uint16>(EContentBrowserItemFlags::Type_MASK)))
			, VirtualPath(Path.empty() ? std::string{} : std::filesystem::path(Path).lexically_normal().generic_string())
		{
			while (VirtualPath.size() > 1 && VirtualPath.back() == '/') VirtualPath.pop_back();
		}
		auto GetItemType() const -> EContentBrowserItemFlags { return Type; }
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
		auto IsValid() const -> bool
		{
			return (Type == EContentBrowserItemFlags::Type_Folder || Type == EContentBrowserItemFlags::Type_File) && VirtualPath.starts_with('/');
		}
		auto GetHash() const noexcept -> size_t { return std::hash<std::string>{}(VirtualPath) ^ (static_cast<size_t>(Type) << 1); }
		friend auto operator==(const FContentBrowserItemDataKey&, const FContentBrowserItemDataKey&) -> bool = default;
	private:
		EContentBrowserItemFlags Type = EContentBrowserItemFlags::None;
		std::string VirtualPath;
	};
	// Folders aggregate across sources. Files remain distinct across source instances.
	class FContentBrowserItemKey : public FContentBrowserItemDataKey
	{
	public:
		FContentBrowserItemKey() = default;
		CONTENTBROWSER_API explicit FContentBrowserItemKey(const FContentBrowserItemData&);
		CONTENTBROWSER_API explicit FContentBrowserItemKey(const FContentBrowserItem&);
		FContentBrowserItemKey(EContentBrowserItemFlags Type, std::string_view Path, FObjectKey Source)
			: FContentBrowserItemDataKey(Type, Path)
			, Owner(GetItemType() == EContentBrowserItemFlags::Type_File ? Source : FObjectKey{}) {}
		auto GetOwnerKey() const -> FObjectKey { return Owner; }
		auto IsValid() const -> bool { return FContentBrowserItemDataKey::IsValid() && (GetItemType() != EContentBrowserItemFlags::Type_File || !Owner.IsNull()); }
		auto GetHash() const noexcept -> size_t { return FContentBrowserItemDataKey::GetHash() ^ (Owner.GetHash() << 2); }
		friend auto operator==(const FContentBrowserItemKey&, const FContentBrowserItemKey&) -> bool = default;
	private:
		FObjectKey Owner;
	};
	// Durin admission identity for a captured contribution, including folder owners.
	struct FContentBrowserItemContributionKey
	{
		FContentBrowserItemDataKey Item;
		FObjectKey Owner;
		uint64 ActivationGeneration = 0, MountRevision = 0;
		auto IsValid() const -> bool { return Item.IsValid() && !Owner.IsNull() && ActivationGeneration != 0; }
		auto GetHash() const noexcept -> size_t
		{
			return Item.GetHash() ^ (Owner.GetHash() << 2) ^ (std::hash<uint64>{}(ActivationGeneration) << 3) ^ (std::hash<uint64>{}(MountRevision) << 4);
		}
		friend auto operator==(const FContentBrowserItemContributionKey&, const FContentBrowserItemContributionKey&) -> bool = default;
	};
	struct FContentBrowserItemKeyHash { auto operator()(const FContentBrowserItemKey& Key) const noexcept -> size_t { return Key.GetHash(); } };
	using FContentBrowserItemKeySet = std::unordered_set<FContentBrowserItemKey, FContentBrowserItemKeyHash>;
	struct FContentBrowserItemContributionKeyHash { auto operator()(const FContentBrowserItemContributionKey& Key) const noexcept -> size_t { return Key.GetHash(); } };
}
