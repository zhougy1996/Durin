#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItem.h"
#include "ContentBrowserAssetDataPayload.h"
#include "ContentBrowserFileDataPayload.h"
#include "Misc/ContentMountPaths.h"
#include "DObject/AssetPath.h"

namespace Durin::ContentBrowser::Private
{
	// Provider-private input to native discovery and storage utilities.
	// Generic items never contain this record.
	struct FStorageItemFixture
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string Name;
		std::string VirtualPath;
		FPackagePath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		std::string Extension;
		FSoftObjectPath RedirectDestination;
		uintmax_t FileSize = 0;
		uintmax_t PackageFileSize = 0;
		uintmax_t BulkFileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		auto GetItemName() const -> const std::string& { return Name; }
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
		operator FContentBrowserItem() const;
	};
}

namespace Durin::ContentBrowser::Private
{
	inline auto MakeFixtureItem(const FStorageItemFixture& Native, bool AssetSource, std::shared_ptr<const FContentBrowserOperationContext> Host = {}) -> FContentBrowserItem
	{
		using namespace AssetDataSource::Private;
		using namespace FileDataSource::Private;
		std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
		if (Native.Kind == EContentBrowserItemKind::Asset || Native.Kind == EContentBrowserItemKind::Redirector)
		{
			auto P=std::make_shared<FContentBrowserAssetFileItemDataPayload>(); P->Location.PhysicalPath=Native.PhysicalPath; FTopLevelAssetPath::TryCreate(Native.VirtualPath, P->AssetPath); P->PackagePath=Native.PackagePath; P->AssetClassName=Native.AssetClassName; P->Extension=Native.Extension; P->RedirectDestination=Native.RedirectDestination; P->FileSize=Native.FileSize; P->PackageFileSize=Native.PackageFileSize; P->BulkFileSize=Native.BulkFileSize; P->LastWriteTime=Native.LastWriteTime; P->bRedirector=Native.Kind==EContentBrowserItemKind::Redirector; P->Host=Host; Payload=P;
		}
		else if (Native.Kind == EContentBrowserItemKind::File)
		{
			auto P=std::make_shared<FContentBrowserFileItemDataPayload>(); P->Location.PhysicalPath=Native.PhysicalPath; P->Extension=Native.Extension; P->FileSize=Native.FileSize; P->LastWriteTime=Native.LastWriteTime; P->Host=Host; Payload=P;
		}
		else if (AssetSource) { auto P=std::make_shared<FContentBrowserAssetFolderItemDataPayload>(); P->Location.PhysicalPath=Native.PhysicalPath; P->Host=Host; Payload=P; }
		else { auto P=std::make_shared<FContentBrowserFolderItemDataPayload>(); P->Location.PhysicalPath=Native.PhysicalPath; P->Host=Host; Payload=P; }
		return {.InternalItems = {FContentBrowserItemData((Native.Kind == EContentBrowserItemKind::Folder ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File) | (AssetSource ? EContentBrowserItemFlags::Category_Asset : EContentBrowserItemFlags::Category_Misc), Native.VirtualPath, Native.Name, {}, std::move(Payload))}};
	}
	inline FStorageItemFixture::operator FContentBrowserItem() const { return MakeFixtureItem(*this, Kind != EContentBrowserItemKind::File); }
}
