#include "Terrain/TerrainWorldCook.h"

#include "Asset/PackageInspection.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 ManifestMagic = 0x464d5754; // TWMF
		constexpr uint32 ManifestRequiredFlags = 1;
		constexpr uint32 MaximumManifestRegions = 4096;
		constexpr uint32 MaximumManifestBytes = 64u * 1024u * 1024u;

		auto TerrainWorldCookFail(ETerrainWorldOutcome Outcome, std::string Message, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			OutOutcome = Outcome;
			OutError = std::move(Message);
			return false;
		}

		auto WriteGuid(FBinaryWriter& Writer, const FGuid& Guid) -> void
		{
			for (uint32 Word : {Guid.A, Guid.B, Guid.C, Guid.D})
				for (int Shift : {24, 16, 8, 0})
					Writer.WriteU8(static_cast<uint8>(Word >> Shift));
		}

		auto ReadGuid(FBinaryReader& Reader, FGuid& OutGuid) -> bool
		{
			std::array<uint32, 4> Words{};
			for (uint32& Word : Words)
				for (int Shift : {24, 16, 8, 0})
				{
					uint8 Byte = 0;
					if (!Reader.ReadU8(Byte)) return false;
					Word |= static_cast<uint32>(Byte) << Shift;
				}
			OutGuid = {Words[0], Words[1], Words[2], Words[3]};
			return true;
		}

		auto WriteHash(FBinaryWriter& Writer, const FXxHash128& Hash) -> void
		{
			Writer.WriteU64(Hash.HashLow);
			Writer.WriteU64(Hash.HashHigh);
		}

		auto ReadHash(FBinaryReader& Reader, FXxHash128& OutHash) -> bool
		{
			return Reader.ReadU64(OutHash.HashLow) && Reader.ReadU64(OutHash.HashHigh);
		}

		auto WriteTile(FBinaryWriter& Writer, const FTerrainTileKey& Tile) -> void
		{
			WriteGuid(Writer, Tile.WorldId.Value);
			Writer.WriteI64(Tile.TileX);
			Writer.WriteI64(Tile.TileY);
			Writer.WriteU16(Tile.SchemeVersion);
		}

		auto ReadTile(FBinaryReader& Reader, FTerrainTileKey& Tile) -> bool
		{
			return ReadGuid(Reader, Tile.WorldId.Value) && Reader.ReadI64(Tile.TileX)
				   && Reader.ReadI64(Tile.TileY) && Reader.ReadU16(Tile.SchemeVersion);
		}

		auto ProductLess(const FTerrainManifestProductEntry& A, const FTerrainManifestProductEntry& B) -> bool
		{
			return std::tuple(A.Tile.TileY, A.Tile.TileX, static_cast<uint8>(A.ProductClass))
				   < std::tuple(B.Tile.TileY, B.Tile.TileX, static_cast<uint8>(B.ProductClass));
		}

		auto RegionLess(const FTerrainManifestRegion& A, const FTerrainManifestRegion& B) -> bool
		{
			return std::pair(A.Region.RegionY, A.Region.RegionX)
				   < std::pair(B.Region.RegionY, B.Region.RegionX);
		}

		auto IsCompleteRegionDirectory(const FTerrainWorldManifest& Manifest, const FTerrainManifestRegion& Region) -> bool
		{
			if (!Region.bInstalled) return Region.Products.empty();
			if (Region.Products.empty() || Region.Products.size() % 5 != 0) return false;
			for (size_t Base = 0; Base < Region.Products.size(); Base += 5)
			{
				const FTerrainManifestProductEntry& First = Region.Products[Base];
				int64 RegionX = 0, RegionY = 0;
				if (!TerrainFloorDiv(First.Tile.TileX, 8, RegionX)
					|| !TerrainFloorDiv(First.Tile.TileY, 8, RegionY)
					|| RegionX != Region.Region.RegionX || RegionY != Region.Region.RegionY) return false;
				for (uint8 Offset = 0; Offset < 5; ++Offset)
				{
					const FTerrainManifestProductEntry& Product = Region.Products[Base + Offset];
					if (Product.Tile != First.Tile || Product.GenerationId != First.GenerationId
						|| Product.ProductClass != static_cast<ETerrainTileProductClass>(Offset + 1)
						|| Product.StoredSize == 0
						|| Product.SegmentOffset > Region.SegmentExtent
						|| Product.StoredSize > Region.SegmentExtent - Product.SegmentOffset
						|| Product.ProductHash.IsZero()
						|| std::ranges::any_of(Product.Dependencies, [](const FXxHash128& Value) { return Value.IsZero(); })) return false;
				}
			}
			if ((Region.SegmentExtent == 0) != Region.SegmentHash.IsZero()) return false;
			return true;
		}

		auto RegionPath(std::string_view Root, const FTerrainRegionKey& Region) -> std::string
		{
			return std::format("{}/Regions/{}_{}", Root, Region.RegionX < 0 ? std::format("N{}", -Region.RegionX) : std::format("P{}", Region.RegionX), Region.RegionY < 0 ? std::format("N{}", -Region.RegionY) : std::format("P{}", Region.RegionY));
		}

		auto IsInstalled(std::span<const FTerrainRegionKey> Installed, const FTerrainRegionKey& Region) -> bool
		{
			return std::ranges::find(Installed, Region) != Installed.end();
		}

		auto MapLoadError(std::string_view Error) -> ETerrainWorldOutcome
		{
			if (Error.find("missing") != std::string_view::npos) return ETerrainWorldOutcome::MissingDependency;
			if (Error.find("target") != std::string_view::npos || Error.find("incompatible") != std::string_view::npos)
				return ETerrainWorldOutcome::Incompatible;
			return ETerrainWorldOutcome::Corrupt;
		}
	} // namespace

	auto GetTerrainRegionKey(const FTerrainTileKey& Tile, FTerrainRegionKey& OutRegion, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		int64 RegionX = 0, RegionY = 0;
		if (!Tile.WorldId.IsValid() || Tile.SchemeVersion != TerrainWorldTileSchemeVersion)
			return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain region lookup tile identity is invalid.", OutOutcome, OutError);
		if (!TerrainFloorDiv(Tile.TileX, 8, RegionX) || !TerrainFloorDiv(Tile.TileY, 8, RegionY))
			return TerrainWorldCookFail(ETerrainWorldOutcome::Overflow, "Terrain region lookup overflowed.", OutOutcome, OutError);
		OutRegion = {Tile.WorldId, RegionX, RegionY, Tile.SchemeVersion};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto EncodeTerrainWorldManifest(const FTerrainWorldManifest& Manifest, FByteArray& OutBytes, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!Manifest.WorldId.IsValid() || Manifest.SchemaVersion != TerrainWorldSchemaVersion
			|| Manifest.TargetPlatform == ECookTargetPlatform::Invalid
			|| Manifest.TargetProfile == ECookTargetProfile::Invalid)
			return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World manifest identity or compatibility is invalid.", OutOutcome, OutError);
		if (Manifest.Regions.size() > MaximumManifestRegions
			|| !std::ranges::is_sorted(Manifest.Regions, RegionLess)
			|| std::adjacent_find(Manifest.Regions.begin(), Manifest.Regions.end(), [](const auto& A, const auto& B) { return A.Region == B.Region; })
				   != Manifest.Regions.end())
			return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World manifest regions exceed their bound or are not sorted.", OutOutcome, OutError);
		FBinaryWriter Records;
		for (const FTerrainManifestRegion& Region : Manifest.Regions)
		{
			if (Region.Region.WorldId != Manifest.WorldId
				|| Region.Region.SchemeVersion != TerrainWorldTileSchemeVersion
				|| Region.VirtualPackagePath.size() > 1024
				|| (Region.bInstalled && Region.VirtualPackagePath.empty())
				|| Region.Products.size() > 64u * 5u
				|| !std::ranges::is_sorted(Region.Products, ProductLess)
				|| !IsCompleteRegionDirectory(Manifest, Region))
				return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World manifest region is invalid or not canonical.", OutOutcome, OutError);
			Records.WriteI64(Region.Region.RegionX);
			Records.WriteI64(Region.Region.RegionY);
			Records.WriteU8(Region.bInstalled ? 1 : 0);
			Records.WriteString(Region.VirtualPackagePath);
			Records.WriteU64(Region.SegmentExtent);
			WriteHash(Records, Region.SegmentHash);
			Records.WriteU32(static_cast<uint32>(Region.Products.size()));
			std::array<uint32, 5> ClassCounts{};
			for (const FTerrainManifestProductEntry& Product : Region.Products)
			{
				const uint8 ProductValue = static_cast<uint8>(Product.ProductClass);
				if (ProductValue < 1 || ProductValue > 5 || ++ClassCounts[ProductValue - 1] > 64
					|| Product.Tile.WorldId != Manifest.WorldId || !Product.GenerationId.IsValid()
					|| Product.Dependencies.size() > TerrainWorldMaximumDependencies)
					return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World manifest product is invalid.", OutOutcome, OutError);
				WriteTile(Records, Product.Tile);
				WriteGuid(Records, Product.GenerationId);
				Records.WriteU8(ProductValue);
				Records.WriteU64(Product.SegmentOffset);
				Records.WriteU64(Product.StoredSize);
				WriteHash(Records, Product.ProductHash);
				Records.WriteU8(static_cast<uint8>(Product.Dependencies.size()));
				for (const FXxHash128& Dependency : Product.Dependencies)
					WriteHash(Records, Dependency);
			}
		}
		const FXxHash128 RecordsHash = FXxHash128::HashBuffer(Records.GetBytes());
		FBinaryWriter Writer;
		Writer.WriteU32(ManifestMagic);
		Writer.WriteU16(TerrainWorldSchemaVersion);
		Writer.WriteU16(0);
		Writer.WriteU32(ManifestRequiredFlags);
		Writer.WriteU32(0);
		WriteGuid(Writer, Manifest.WorldId.Value);
		Writer.WriteU32(static_cast<uint32>(Manifest.TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(Manifest.TargetProfile));
		Writer.WriteU32(static_cast<uint32>(Manifest.Regions.size()));
		Writer.WriteU64(Records.GetBytes().size());
		WriteHash(Writer, RecordsHash);
		Writer.WriteBytes(Records.GetBytes());
		if (Writer.GetBytes().size() > MaximumManifestBytes)
			return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World manifest exceeds its byte bound.", OutOutcome, OutError);
		OutBytes = Writer.TakeBytes();
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto DecodeTerrainWorldManifest(std::span<const std::byte> Bytes, const FTerrainWorldId& ExpectedWorld, ECookTargetPlatform ExpectedPlatform, ECookTargetProfile ExpectedProfile, FTerrainWorldManifest& OutManifest, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutManifest = {};
		uint32 Magic = 0;
		uint16 Version = 0;
		if (Bytes.size() < 6 || !ReadLittleEndianAt(Bytes, 0, Magic)
			|| !ReadLittleEndianAt(Bytes, 4, Version))
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest is truncated.", OutOutcome, OutError);
		if (Magic != ManifestMagic)
			return TerrainWorldCookFail(ETerrainWorldOutcome::UnsupportedLegacySchema, "Terrain World manifest magic is unsupported.", OutOutcome, OutError);
		if (Version != TerrainWorldSchemaVersion)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Incompatible, "Terrain World manifest schema is incompatible.", OutOutcome, OutError);
		if (Bytes.size() > MaximumManifestBytes)
			return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World manifest exceeds its byte bound.", OutOutcome, OutError);
		FBinaryReader Reader(Bytes);
		uint32 HeaderMagic = 0, Required = 0, Optional = 0, Platform = 0, Profile = 0, RegionCount = 0;
		uint16 HeaderVersion = 0, Reserved = 0;
		uint64 RecordsBytes = 0;
		FXxHash128 RecordsHash;
		FTerrainWorldManifest Candidate;
		if (!Reader.ReadU32(HeaderMagic) || !Reader.ReadU16(HeaderVersion)
			|| !Reader.ReadU16(Reserved) || !Reader.ReadU32(Required) || !Reader.ReadU32(Optional)
			|| !ReadGuid(Reader, Candidate.WorldId.Value) || !Reader.ReadU32(Platform)
			|| !Reader.ReadU32(Profile) || !Reader.ReadU32(RegionCount)
			|| !Reader.ReadU64(RecordsBytes) || !ReadHash(Reader, RecordsHash))
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest header is truncated.", OutOutcome, OutError);
		if (HeaderMagic != Magic || HeaderVersion != Version || Reserved != 0
			|| Required != ManifestRequiredFlags || Optional != 0 || Candidate.WorldId != ExpectedWorld
			|| Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile) || RegionCount > MaximumManifestRegions
			|| RecordsBytes != Reader.GetRemainingBytes())
			return TerrainWorldCookFail(ETerrainWorldOutcome::Incompatible, "Terrain World manifest identity, target, flags, or sizes are incompatible.", OutOutcome, OutError);
		std::span<const std::byte> RecordSpan;
		if (!Reader.ReadRegion(RecordSpan, RecordsBytes, MaximumManifestBytes)
			|| FXxHash128::HashBuffer(RecordSpan) != RecordsHash)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest record checksum is invalid.", OutOutcome, OutError);
		FBinaryReader Records(RecordSpan);
		Candidate.SchemaVersion = Version;
		Candidate.TargetPlatform = static_cast<ECookTargetPlatform>(Platform);
		Candidate.TargetProfile = static_cast<ECookTargetProfile>(Profile);
		Candidate.Regions.reserve(RegionCount);
		for (uint32 RegionIndex = 0; RegionIndex < RegionCount; ++RegionIndex)
		{
			FTerrainManifestRegion Region;
			uint8 Installed = 0;
			uint32 ProductCount = 0;
			Region.Region.WorldId = Candidate.WorldId;
			Region.Region.SchemeVersion = TerrainWorldTileSchemeVersion;
			if (!Records.ReadI64(Region.Region.RegionX) || !Records.ReadI64(Region.Region.RegionY)
				|| !Records.ReadU8(Installed) || Installed > 1
				|| !Records.ReadString(Region.VirtualPackagePath, 1024)
				|| !Records.ReadU64(Region.SegmentExtent)
				|| !ReadHash(Records, Region.SegmentHash)
				|| !Records.ReadU32(ProductCount) || ProductCount > 64u * 5u)
				return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest region record is invalid.", OutOutcome, OutError);
			Region.bInstalled = Installed != 0;
			Region.Products.reserve(ProductCount);
			for (uint32 ProductIndex = 0; ProductIndex < ProductCount; ++ProductIndex)
			{
				FTerrainManifestProductEntry Product;
				uint8 ProductClass = 0, DependencyCount = 0;
				if (!ReadTile(Records, Product.Tile) || !ReadGuid(Records, Product.GenerationId)
					|| !Records.ReadU8(ProductClass) || ProductClass < 1 || ProductClass > 5
					|| !Records.ReadU64(Product.SegmentOffset)
					|| !Records.ReadU64(Product.StoredSize)
					|| !ReadHash(Records, Product.ProductHash)
					|| !Records.ReadU8(DependencyCount) || DependencyCount > TerrainWorldMaximumDependencies)
					return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest product record is invalid.", OutOutcome, OutError);
				Product.ProductClass = static_cast<ETerrainTileProductClass>(ProductClass);
				Product.Dependencies.resize(DependencyCount);
				for (FXxHash128& Dependency : Product.Dependencies)
					if (!ReadHash(Records, Dependency))
						return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest dependency is truncated.", OutOutcome, OutError);
				Region.Products.push_back(std::move(Product));
			}
			if (!std::ranges::is_sorted(Region.Products, ProductLess)
				|| !IsCompleteRegionDirectory(Candidate, Region)
				|| (!Candidate.Regions.empty() && !RegionLess(Candidate.Regions.back(), Region)))
				return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest records are not canonically ordered.", OutOutcome, OutError);
			Candidate.Regions.push_back(std::move(Region));
		}
		if (!Records.IsAtEnd())
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World manifest has trailing record bytes.", OutOutcome, OutError);
		OutManifest = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ContributeTerrainWorldToCook(const FTerrainWorldCookRequest& Request, FCookContext& Cook, FTerrainWorldManifest& OutManifest, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutManifest = {};
		if (!Request.WorldId.IsValid()
			|| Request.VirtualWorldRoot.empty() || Request.VirtualWorldRoot.back() == '/'
			|| Request.PackageTemplateBytes.empty() || Request.TargetPlatform == ECookTargetPlatform::Invalid
			|| Request.TargetProfile == ECookTargetProfile::Invalid
			|| Cook.GetTargetPlatform() != Request.TargetPlatform
			|| Cook.GetTargetProfile() != Request.TargetProfile)
			return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World Cook request is invalid.", OutOutcome, OutError);
		if (Request.Generations.size() > 262144)
			return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World Cook exceeds the profile tile ceiling.", OutOutcome, OutError);
		FTerrainWorldManifest Manifest{Request.WorldId, TerrainWorldSchemaVersion, Request.TargetPlatform, Request.TargetProfile};
		for (const FTerrainTileGeneration& Generation : Request.Generations)
		{
			if (Generation.Tile.WorldId != Request.WorldId || !Generation.GenerationId.IsValid())
				return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World Cook generation identity is invalid.", OutOutcome, OutError);
			FTerrainRegionKey RegionKey;
			if (!GetTerrainRegionKey(Generation.Tile, RegionKey, OutOutcome, OutError)) return false;
			auto It = std::ranges::find_if(Manifest.Regions, [&](const FTerrainManifestRegion& Value) { return Value.Region == RegionKey; });
			if (It == Manifest.Regions.end())
			{
				Manifest.Regions.push_back({RegionKey, IsInstalled(Request.InstalledRegions, RegionKey), RegionPath(Request.VirtualWorldRoot, RegionKey), {}});
				It = std::prev(Manifest.Regions.end());
			}
			if (!It->bInstalled) continue;
			for (const FTerrainTileProduct& Product : Generation.Products)
			{
				FTerrainTileProduct Validated;
				if (Product.Tile != Generation.Tile || Product.GenerationId != Generation.GenerationId
					|| !DecodeTerrainTileProduct(Product.Bytes, Product.ProductClass, Validated, OutOutcome, OutError))
					return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Terrain World Cook requires a complete validated generation.", OutOutcome, OutError);
			}
		}
		std::ranges::sort(Manifest.Regions, RegionLess);
		for (size_t Index = 0; Index < Request.InstalledRegions.size(); ++Index)
		{
			const FTerrainRegionKey& Selected = Request.InstalledRegions[Index];
			if (Selected.WorldId != Request.WorldId
				|| Selected.SchemeVersion != TerrainWorldTileSchemeVersion
				|| std::find(Request.InstalledRegions.begin(), Request.InstalledRegions.begin() + Index, Selected)
					   != Request.InstalledRegions.begin() + Index
				|| std::ranges::find(Manifest.Regions, Selected, &FTerrainManifestRegion::Region)
					   == Manifest.Regions.end())
				return TerrainWorldCookFail(ETerrainWorldOutcome::MissingDependency, "Terrain World Cook selected a missing, duplicate, or incompatible region.", OutOutcome, OutError);
		}
		for (FTerrainManifestRegion& Region : Manifest.Regions)
		{
			if (!Region.bInstalled) continue;
			FByteArray Segment;
			for (const FTerrainTileGeneration& Generation : Request.Generations)
			{
				FTerrainRegionKey Key;
				if (!GetTerrainRegionKey(Generation.Tile, Key, OutOutcome, OutError)) return false;
				if (Key != Region.Region) continue;
				for (const FTerrainTileProduct& Product : Generation.Products)
				{
					const uint64 Offset = (Segment.size() + 15u) & ~uint64{15};
					Segment.resize(static_cast<size_t>(Offset), std::byte{0});
					Region.Products.push_back({.Tile = Product.Tile, .GenerationId = Product.GenerationId, .ProductClass = Product.ProductClass, .SegmentOffset = Offset, .StoredSize = Product.Bytes.size(), .ProductHash = FXxHash128::HashBuffer(Product.Bytes), .Dependencies = Product.Dependencies});
					Segment.insert(Segment.end(), Product.Bytes.begin(), Product.Bytes.end());
				}
			}
			if (Region.Products.size() > 64u * 5u)
				return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World region exceeds 64 products per class.", OutOutcome, OutError);
			if (Segment.size() > TerrainWorldMaximumRegionLogicalBytes
				|| Segment.size() > TerrainWorldMaximumRegionStoredBytes)
				return TerrainWorldCookFail(ETerrainWorldOutcome::BudgetRejected, "Terrain World region exceeds its logical byte ceiling.", OutOutcome, OutError);
			std::ranges::sort(Region.Products, ProductLess);
			Region.SegmentExtent = Segment.size();
			Region.SegmentHash = FXxHash128::HashBuffer(Segment);
			if (!Cook.AddRawPackage(Region.VirtualPackagePath, Request.PackageTemplateBytes, std::move(Segment), &OutError))
				return TerrainWorldCookFail(ETerrainWorldOutcome::PublicationFailed, "Terrain World region package could not be staged: " + OutError, OutOutcome, OutError);
		}
		FByteArray ManifestBytes;
		if (!EncodeTerrainWorldManifest(Manifest, ManifestBytes, OutOutcome, OutError)) return false;
		if (!Cook.AddRawPackage(Request.VirtualWorldRoot + "/Manifest", Request.PackageTemplateBytes, std::move(ManifestBytes), &OutError))
			return TerrainWorldCookFail(ETerrainWorldOutcome::PublicationFailed, "Terrain World Cook contribution failed: " + OutError, OutOutcome, OutError);
		OutManifest = std::move(Manifest);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto CookTerrainWorld(const FTerrainWorldCookRequest& Request, FTerrainWorldManifest& OutManifest, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (Request.CookRoot.empty() || !Request.CookRoot.is_absolute())
			return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Terrain World Cook output root is invalid.", OutOutcome, OutError);
		FCookContext Cook(Request.CookRoot, Request.TargetPlatform, Request.TargetProfile);
		if (!ContributeTerrainWorldToCook(
				Request, Cook, OutManifest, OutOutcome, OutError
			)) return false;
		if (!Cook.Publish(&OutError))
			return TerrainWorldCookFail(ETerrainWorldOutcome::PublicationFailed, "Terrain World Cook publication failed: " + OutError, OutOutcome, OutError);
		return true;
	}

	auto LoadCookedTerrainWorldManifest(
		const FAssetRuntimeConfiguration& RuntimeConfiguration,
		std::string_view VirtualWorldRoot,
		const FTerrainWorldId& WorldId,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		std::shared_ptr<const FTerrainWorldManifest>& OutManifest,
		ETerrainWorldOutcome& OutOutcome,
		std::string& OutError
	) -> bool
	{
		OutManifest.reset();
		std::filesystem::path PackagePath, BulkPath;
		const std::string VirtualPath = std::string(VirtualWorldRoot) + "/Manifest";
		if (!RuntimeConfiguration.RequiresCookedPayload()
			|| !ResolveCookedPackagePath(RuntimeConfiguration.GetCookRoot(), VirtualPath, PackagePath, &OutError)
			|| !ResolveCookedCompanionPath(RuntimeConfiguration.GetCookRoot(), PackagePath, BulkPath, &OutError))
			return TerrainWorldCookFail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		FByteArray Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, BulkPath))
			return TerrainWorldCookFail(ETerrainWorldOutcome::MissingDependency, "Cooked Terrain World manifest segment is missing.", OutOutcome, OutError);
		FTerrainWorldManifest Decoded;
		if (!DecodeTerrainWorldManifest(Bytes, WorldId, ExpectedPlatform, ExpectedProfile, Decoded, OutOutcome, OutError)) return false;
		OutManifest = std::make_shared<const FTerrainWorldManifest>(std::move(Decoded));
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto LoadCookedTerrainProduct(const FAssetRuntimeConfiguration& RuntimeConfiguration, const std::shared_ptr<const FTerrainWorldManifest>& Manifest, const FTerrainTileKey& Tile, const FGuid& GenerationId, ETerrainTileProductClass ProductClass, FTerrainCookedProductHandle& OutHandle, ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutHandle = {};
		if (!Manifest || Manifest->WorldId != Tile.WorldId)
			return TerrainWorldCookFail(ETerrainWorldOutcome::InvalidDefinition, "Cooked Terrain product request does not match its manifest.", OutOutcome, OutError);
		FTerrainRegionKey RegionKey;
		if (!GetTerrainRegionKey(Tile, RegionKey, OutOutcome, OutError)) return false;
		const auto Region = std::ranges::find_if(Manifest->Regions, [&](const FTerrainManifestRegion& Value) { return Value.Region == RegionKey; });
		if (Region == Manifest->Regions.end() || !Region->bInstalled)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Unavailable, "Cooked Terrain region is not installed.", OutOutcome, OutError);
		const auto Entry = std::ranges::find_if(Region->Products, [&](const FTerrainManifestProductEntry& Value) {
			return Value.Tile == Tile && Value.GenerationId == GenerationId
				   && Value.ProductClass == ProductClass;
		});
		if (Entry == Region->Products.end())
			return TerrainWorldCookFail(ETerrainWorldOutcome::MissingDependency, "Cooked Terrain product is missing.", OutOutcome, OutError);
		std::filesystem::path PackagePath, BulkPath;
		if (!ResolveCookedPackagePath(RuntimeConfiguration.GetCookRoot(), Region->VirtualPackagePath, PackagePath, &OutError)
			|| !ResolveCookedCompanionPath(RuntimeConfiguration.GetCookRoot(), PackagePath, BulkPath, &OutError))
			return TerrainWorldCookFail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		FByteArray Segment;
		if (!FFileHelper::LoadFileToArray(Segment, BulkPath))
			return TerrainWorldCookFail(ETerrainWorldOutcome::MissingDependency, "Cooked Terrain region segment is missing.", OutOutcome, OutError);
		if (Segment.size() != Region->SegmentExtent
			|| FXxHash128::HashBuffer(Segment) != Region->SegmentHash
			|| Entry->SegmentOffset > Segment.size()
			|| Entry->StoredSize > Segment.size() - Entry->SegmentOffset)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Cooked Terrain region segment metadata is invalid.", OutOutcome, OutError);
		const std::span<const std::byte> ProductBytes = std::span(Segment).subspan(
			static_cast<size_t>(Entry->SegmentOffset), static_cast<size_t>(Entry->StoredSize)
		);
		if (FXxHash128::HashBuffer(ProductBytes) != Entry->ProductHash)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Cooked Terrain product manifest checksum is invalid.", OutOutcome, OutError);
		FTerrainTileProduct Product;
		if (!DecodeTerrainTileProduct(ProductBytes, ProductClass, Product, OutOutcome, OutError)) return false;
		if (Product.Tile != Tile || Product.GenerationId != GenerationId
			|| Product.Dependencies != Entry->Dependencies)
			return TerrainWorldCookFail(ETerrainWorldOutcome::Corrupt, "Cooked Terrain product identity or dependencies do not match the manifest.", OutOutcome, OutError);
		OutHandle = {Manifest, FSharedByteBuffer::Copy(ProductBytes), *Entry};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}
} // namespace Durin
