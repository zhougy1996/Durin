#include "AssetTools/AssetRenameReferences.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFile.h"
#include "Misc/FileTime.h"
#include "Asset/AssetWriteResult.h"

namespace Durin::AssetToolsPrivate
{
	namespace
	{
		auto ReplacePath(const FSoftObjectPath& Path, const FAssetRenameData& Mapping) -> std::optional<FSoftObjectPath>
		{
			const auto Text = Path.ToString(), Prefix = Mapping.OldObjectPath.ToString();
			FSoftObjectPath Head, After;
			if (Text.size() < Prefix.size() || (Text.size() > Prefix.size() && Text[Prefix.size()] != ':' && Text[Prefix.size()] != '.')
				|| !FSoftObjectPath::TryCreate(Text.substr(0, Prefix.size()), Head) || Head != Mapping.OldObjectPath) return {};
			if (!FSoftObjectPath::TryCreate(Mapping.NewObjectPath.ToString() + Text.substr(Prefix.size()), After)) return {};
			return After;
		}
	}

	auto FAssetReferenceFixup::GetRegistry() -> const FAssetRegistrySnapshot&
	{
		if (!Registry || Registry->Revision != GetAssetCatalogRevision()) Registry = CaptureAssetRegistrySnapshot();
		return *Registry;
	}

	auto FAssetReferenceFixup::GetPackageReferences(const FPackageData& Data) -> std::expected<const std::vector<FAssetReferenceEdge>*, UnifiedError::FError>
	{
		if (const auto Found = Packages.find(Data.PackagePath); Found != Packages.end())
		{
			const auto& Cached = Found->second;
			std::error_code Error;
			const auto Size = std::filesystem::file_size(Data.PhysicalPath, Error);
			const auto Time = Error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(Data.PhysicalPath, Error);
			if (!Error && Cached.PhysicalPath == Data.PhysicalPath && Cached.Fingerprint.ReaderVersion == Data.FormatVersion
				&& Cached.Fingerprint.FileSize == Size && Cached.Fingerprint.LastWriteTimeTicks == FileTime::ToStableTicks(Time))
			{
				const auto Hash = FFileHelper::HashFileXx128(Data.PhysicalPath);
				if (!Hash) return std::unexpected(ToError(Hash.error()));
				if (*Hash == Cached.Fingerprint.ContentHash) return &Cached.Occurrences;
			}
		}
		FAssetPackageInspection Inspection;
		if (auto Read = InspectAssetPackage(Data.PhysicalPath, Data.PackagePath, Inspection); !Read) return std::unexpected(Read.error());
		FPackageReferences Facts{Data.PhysicalPath, Inspection.Fingerprint};
		if (auto Read = ExtractAssetReferences(Data.PackagePath, Inspection, Facts.Occurrences); !Read) return std::unexpected(Read.error());
		return &Packages.insert_or_assign(Data.PackagePath, std::move(Facts)).first->second.Occurrences;
	}

	auto FAssetReferenceFixup::GetStores() -> std::expected<const FAssetReferenceStoreCapture*, UnifiedError::FError>
	{
		if (!Stores)
		{
			auto Registrations = CaptureAssetReferenceStoreRegistrations();
			FAssetReferenceStoreCapture Captured;
			if (auto Read = CaptureAssetReferenceStores(Captured); !Read) return std::unexpected(Read.error());
			if (Captured.RegistryRevision != Registrations.Revision || Captured.Stores.size() != Registrations.Stores.size())
				return std::unexpected(UnifiedError::FError{AssetWriteErrors::StaleData, "Reference store registrations changed during collection."});
			StoreRegistrations = std::move(Registrations);
			Stores = std::move(Captured);
		}
		if (GetAssetReferenceStoreRevision() != Stores->RegistryRevision)
			return std::unexpected(UnifiedError::FError{AssetWriteErrors::StaleData, "Reference store registrations changed after collection."});
		return &*Stores;
	}

	auto CollectSoftObjectPathFixups(DPackage* Package, std::span<const FAssetRenameData> Mappings,
		std::vector<FSoftObjectPathFixup>& Out) -> std::expected<void, UnifiedError::FError>
	{
		std::unordered_set<FPackagePath> Sources;
		std::unordered_set<FSoftObjectPtr*> Seen;
		for (const auto& Fixup : Out) Seen.insert(Fixup.Value);
		for (const auto& Mapping : Mappings) Sources.insert(Mapping.OldObjectPath.GetPackagePath());
		for (const auto& Source : Sources)
		{
			std::vector<FSoftObjectPtr*> Values;
			if (auto Read = CollectLoadedAssetPackageSoftReferences(Package, Source, Values); !Read) return Read;
			for (auto* Value : Values)
			{
				if (!Value || !Seen.insert(Value).second) continue;
				for (const auto& Mapping : Mappings)
					if (auto After = ReplacePath(Value->ToSoftObjectPath(), Mapping))
					{ Out.push_back({Value, Value->ToSoftObjectPath(), *After, Package}); break; }
			}
		}
		return {};
	}
}
