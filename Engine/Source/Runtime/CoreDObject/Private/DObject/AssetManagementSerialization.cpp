#include "DObject/AssetManagementSerialization.h"

#include "DObject/AssetBundleData.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	// Versioned optional management projection, independent of reference edges.
	auto WriteAssetManagementMetadata(FBinaryWriter& Writer, const FPrimaryAssetId& Id,
		const FAssetBundleData& Data) -> void
	{
		Writer.WriteU32(1);
		Writer.WriteString(Id.ToString());
		Writer.WriteU32(static_cast<uint32>(Data.Bundles.size()));
		for (const auto& Entry : Data.Bundles)
		{
			Writer.WriteString(Entry.BundleName.ToString());
			Writer.WriteU32(static_cast<uint32>(Entry.BundleAssets.size()));
			for (const auto& Path : Entry.BundleAssets) Writer.WriteString(Path.ToString());
		}
	}
	auto ReadAssetManagementMetadata(FBinaryReader& Reader, FPrimaryAssetId& Id,
		FAssetBundleData& Data) -> bool
	{
		FPrimaryAssetId ParsedId;
		FAssetBundleData ParsedData;
		uint32 Version = 0, Count = 0;
		std::string Text;
		if (!Reader.ReadU32(Version) || Version != 1 || !Reader.ReadString(Text)
			|| (!Text.empty() && !FPrimaryAssetId::TryParse(Text, ParsedId))
			|| !Reader.ReadU32(Count) || Count > 65536) return false;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			uint32 Paths = 0;
			if (!Reader.ReadString(Text) || Text.empty() || !Reader.ReadU32(Paths) || Paths > 65536) return false;
			FAssetBundleEntry Entry{FName(Text), {}};
			for (uint32 PathIndex = 0; PathIndex < Paths; ++PathIndex)
			{
				FTopLevelAssetPath Path;
				if (!Reader.ReadString(Text) || !FTopLevelAssetPath::TryCreate(Text, Path)) return false;
				Entry.BundleAssets.push_back(Path);
			}
			ParsedData.Bundles.push_back(std::move(Entry));
		}
		if (!ParsedData.IsValid()) return false;
		Id = ParsedId; Data = std::move(ParsedData);
		return true;
	}
}
