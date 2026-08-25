#include "Terrain/TerrainWorldCook.h"

#include "Asset/PackageInspection.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Build
{
	namespace
	{
		constexpr uint32 ManifestMagic = 0x464d5754; // TWMF
		constexpr uint32 ManifestRequiredFlags = 1;
		constexpr uint32 MaximumManifestRegions = 4096;
		constexpr uint32 MaximumManifestBytes = 64u * 1024u * 1024u;

		auto Fail(ETerrainWorldOutcome Outcome, std::string Message,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			OutOutcome = Outcome;
			OutError = std::move(Message);
			return false;
		}

		auto WriteGuid(FBinaryWriter& Writer, const FGuid& Guid) -> void
		{
			for (uint32 Word : {Guid.A, Guid.B, Guid.C, Guid.D})
				for (int Shift : {24, 16, 8, 0}) Writer.WriteU8(static_cast<uint8>(Word >> Shift));
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

		auto WriteDescriptor(FBinaryWriter& Writer, const Asset::FCookedPayloadDescriptor& Value) -> void
		{
			WriteGuid(Writer, Value.PayloadId);
			Writer.WriteU32(Value.LocationKind);
			Writer.WriteU64(Value.Offset);
			Writer.WriteU64(Value.StoredSize);
			Writer.WriteU64(Value.UncompressedSize);
			Writer.WriteU32(Value.Alignment);
			Writer.WriteU64(Value.PayloadHashLow);
			Writer.WriteU64(Value.PayloadHashHigh);
			Writer.WriteU32(Value.PayloadSchemaVersion);
			Writer.WriteU32(Value.TargetPlatform);
			Writer.WriteU32(Value.TargetProfile);
			Writer.WriteU32(Value.CompressionMethod);
		}

		auto ReadDescriptor(FBinaryReader& Reader, Asset::FCookedPayloadDescriptor& Value) -> bool
		{
			return ReadGuid(Reader, Value.PayloadId) && Reader.ReadU32(Value.LocationKind)
				&& Reader.ReadU64(Value.Offset) && Reader.ReadU64(Value.StoredSize)
				&& Reader.ReadU64(Value.UncompressedSize) && Reader.ReadU32(Value.Alignment)
				&& Reader.ReadU64(Value.PayloadHashLow) && Reader.ReadU64(Value.PayloadHashHigh)
				&& Reader.ReadU32(Value.PayloadSchemaVersion) && Reader.ReadU32(Value.TargetPlatform)
				&& Reader.ReadU32(Value.TargetProfile) && Reader.ReadU32(Value.CompressionMethod);
		}

		auto ProductLess(const FTerrainManifestProductEntry& A,
			const FTerrainManifestProductEntry& B) -> bool
		{
			return std::tuple(A.Tile.TileY, A.Tile.TileX, static_cast<uint8>(A.ProductClass))
				< std::tuple(B.Tile.TileY, B.Tile.TileX, static_cast<uint8>(B.ProductClass));
		}

		auto RegionLess(const FTerrainManifestRegion& A, const FTerrainManifestRegion& B) -> bool
		{
			return std::pair(A.Region.RegionY, A.Region.RegionX)
				< std::pair(B.Region.RegionY, B.Region.RegionX);
		}

		auto IsCompleteRegionDirectory(const FTerrainWorldManifest& Manifest,
			const FTerrainManifestRegion& Region) -> bool
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
					const Asset::FCookedPayloadDescriptor& Descriptor = Product.Descriptor;
					if (Product.Tile != First.Tile || Product.GenerationId != First.GenerationId
						|| Product.ProductClass != static_cast<ETerrainTileProductClass>(Offset + 1)
						|| !Descriptor.PayloadId.IsValid()
						|| Descriptor.LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
						|| Descriptor.StoredSize == 0 || Descriptor.UncompressedSize == 0
						|| Descriptor.PayloadSchemaVersion != TerrainWorldSchemaVersion
						|| Descriptor.TargetPlatform != static_cast<uint32>(Manifest.TargetPlatform)
						|| Descriptor.TargetProfile != static_cast<uint32>(Manifest.TargetProfile)
						|| Product.ProductHash.IsZero()
						|| std::ranges::any_of(Product.Dependencies,
							[](const FXxHash128& Value) { return Value.IsZero(); })) return false;
				}
			}
			return true;
		}

		auto ProductPayloadId(const FTerrainTileProduct& Product) -> FGuid
		{
			FBinaryWriter Writer;
			WriteTile(Writer, Product.Tile);
			WriteGuid(Writer, Product.GenerationId);
			Writer.WriteU8(static_cast<uint8>(Product.ProductClass));
			const FXxHash128 Hash = FXxHash128::HashBuffer(Writer.GetBytes());
			return {static_cast<uint32>(Hash.HashHigh >> 32), static_cast<uint32>(Hash.HashHigh),
				static_cast<uint32>(Hash.HashLow >> 32), static_cast<uint32>(Hash.HashLow)};
		}

		auto ManifestPayloadId(const FTerrainWorldId& WorldId) -> FGuid
		{
			FBinaryWriter Writer;
			WriteGuid(Writer, WorldId.Value);
			Writer.WriteString("TerrainWorldManifest");
			const FXxHash128 Hash = FXxHash128::HashBuffer(Writer.GetBytes());
			return {static_cast<uint32>(Hash.HashHigh >> 32), static_cast<uint32>(Hash.HashHigh),
				static_cast<uint32>(Hash.HashLow >> 32), static_cast<uint32>(Hash.HashLow)};
		}

		auto RegionPath(std::string_view Root, const FTerrainRegionKey& Region) -> std::string
		{
			return std::format("{}/Regions/{}_{}", Root,
				Region.RegionX < 0 ? std::format("N{}", -Region.RegionX) : std::format("P{}", Region.RegionX),
				Region.RegionY < 0 ? std::format("N{}", -Region.RegionY) : std::format("P{}", Region.RegionY));
		}

		auto IsInstalled(std::span<const FTerrainRegionKey> Installed,
			const FTerrainRegionKey& Region) -> bool
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
	}

	auto GetTerrainRegionKey(const FTerrainTileKey& Tile, FTerrainRegionKey& OutRegion,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		int64 RegionX = 0, RegionY = 0;
		if (!Tile.WorldId.IsValid() || Tile.SchemeVersion != TerrainWorldTileSchemeVersion)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain region lookup tile identity is invalid.", OutOutcome, OutError);
		if (!TerrainFloorDiv(Tile.TileX, 8, RegionX) || !TerrainFloorDiv(Tile.TileY, 8, RegionY))
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain region lookup overflowed.", OutOutcome, OutError);
		OutRegion = {Tile.WorldId, RegionX, RegionY, Tile.SchemeVersion};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto EncodeTerrainWorldManifest(const FTerrainWorldManifest& Manifest,
		std::vector<std::byte>& OutBytes, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!Manifest.WorldId.IsValid() || Manifest.SchemaVersion != TerrainWorldSchemaVersion
			|| Manifest.TargetPlatform == Asset::ECookTargetPlatform::Invalid
			|| Manifest.TargetProfile == Asset::ECookTargetProfile::Invalid)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain World manifest identity or compatibility is invalid.", OutOutcome, OutError);
		if (Manifest.Regions.size() > MaximumManifestRegions
			|| !std::ranges::is_sorted(Manifest.Regions, RegionLess)
			|| std::adjacent_find(Manifest.Regions.begin(), Manifest.Regions.end(),
				[](const auto& A, const auto& B) { return A.Region == B.Region; })
				!= Manifest.Regions.end())
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World manifest regions exceed their bound or are not sorted.", OutOutcome, OutError);
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
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World manifest region is invalid or not canonical.", OutOutcome, OutError);
			Records.WriteI64(Region.Region.RegionX);
			Records.WriteI64(Region.Region.RegionY);
			Records.WriteU8(Region.bInstalled ? 1 : 0);
			Records.WriteString(Region.VirtualPackagePath);
			Records.WriteU32(static_cast<uint32>(Region.Products.size()));
			std::array<uint32, 5> ClassCounts{};
			for (const FTerrainManifestProductEntry& Product : Region.Products)
			{
				const uint8 ProductValue = static_cast<uint8>(Product.ProductClass);
				if (ProductValue < 1 || ProductValue > 5 || ++ClassCounts[ProductValue - 1] > 64
					|| Product.Tile.WorldId != Manifest.WorldId || !Product.GenerationId.IsValid()
					|| Product.Dependencies.size() > TerrainWorldMaximumDependencies)
					return Fail(ETerrainWorldOutcome::InvalidDefinition,
						"Terrain World manifest product is invalid.", OutOutcome, OutError);
				WriteTile(Records, Product.Tile);
				WriteGuid(Records, Product.GenerationId);
				Records.WriteU8(ProductValue);
				WriteDescriptor(Records, Product.Descriptor);
				WriteHash(Records, Product.ProductHash);
				Records.WriteU8(static_cast<uint8>(Product.Dependencies.size()));
				for (const FXxHash128& Dependency : Product.Dependencies) WriteHash(Records, Dependency);
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
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World manifest exceeds its byte bound.", OutOutcome, OutError);
		OutBytes = Writer.TakeBytes();
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto DecodeTerrainWorldManifest(std::span<const std::byte> Bytes,
		const FTerrainWorldId& ExpectedWorld, Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile, FTerrainWorldManifest& OutManifest,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutManifest = {};
		uint32 Magic = 0;
		uint16 Version = 0;
		if (Bytes.size() < 6 || !ReadLittleEndianAt(Bytes, 0, Magic)
			|| !ReadLittleEndianAt(Bytes, 4, Version))
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain World manifest is truncated.", OutOutcome, OutError);
		if (Magic != ManifestMagic)
			return Fail(ETerrainWorldOutcome::UnsupportedLegacySchema,
				"Terrain World manifest magic is unsupported.", OutOutcome, OutError);
		if (Version != TerrainWorldSchemaVersion)
			return Fail(ETerrainWorldOutcome::Incompatible,
				"Terrain World manifest schema is incompatible.", OutOutcome, OutError);
		if (Bytes.size() > MaximumManifestBytes)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World manifest exceeds its byte bound.", OutOutcome, OutError);
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
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain World manifest header is truncated.", OutOutcome, OutError);
		if (HeaderMagic != Magic || HeaderVersion != Version || Reserved != 0
			|| Required != ManifestRequiredFlags || Optional != 0 || Candidate.WorldId != ExpectedWorld
			|| Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile) || RegionCount > MaximumManifestRegions
			|| RecordsBytes != Reader.GetRemainingBytes())
			return Fail(ETerrainWorldOutcome::Incompatible,
				"Terrain World manifest identity, target, flags, or sizes are incompatible.", OutOutcome, OutError);
		std::span<const std::byte> RecordSpan;
		if (!Reader.ReadRegion(RecordSpan, RecordsBytes, MaximumManifestBytes)
			|| FXxHash128::HashBuffer(RecordSpan) != RecordsHash)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain World manifest record checksum is invalid.", OutOutcome, OutError);
		FBinaryReader Records(RecordSpan);
		Candidate.SchemaVersion = Version;
		Candidate.TargetPlatform = static_cast<Asset::ECookTargetPlatform>(Platform);
		Candidate.TargetProfile = static_cast<Asset::ECookTargetProfile>(Profile);
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
				|| !Records.ReadU32(ProductCount) || ProductCount > 64u * 5u)
				return Fail(ETerrainWorldOutcome::Corrupt,
					"Terrain World manifest region record is invalid.", OutOutcome, OutError);
			Region.bInstalled = Installed != 0;
			Region.Products.reserve(ProductCount);
			for (uint32 ProductIndex = 0; ProductIndex < ProductCount; ++ProductIndex)
			{
				FTerrainManifestProductEntry Product;
				uint8 ProductClass = 0, DependencyCount = 0;
				if (!ReadTile(Records, Product.Tile) || !ReadGuid(Records, Product.GenerationId)
					|| !Records.ReadU8(ProductClass) || ProductClass < 1 || ProductClass > 5
					|| !ReadDescriptor(Records, Product.Descriptor)
					|| !ReadHash(Records, Product.ProductHash)
					|| !Records.ReadU8(DependencyCount) || DependencyCount > TerrainWorldMaximumDependencies)
					return Fail(ETerrainWorldOutcome::Corrupt,
						"Terrain World manifest product record is invalid.", OutOutcome, OutError);
				Product.ProductClass = static_cast<ETerrainTileProductClass>(ProductClass);
				Product.Dependencies.resize(DependencyCount);
				for (FXxHash128& Dependency : Product.Dependencies)
					if (!ReadHash(Records, Dependency))
						return Fail(ETerrainWorldOutcome::Corrupt,
							"Terrain World manifest dependency is truncated.", OutOutcome, OutError);
				Region.Products.push_back(std::move(Product));
			}
			if (!std::ranges::is_sorted(Region.Products, ProductLess)
				|| !IsCompleteRegionDirectory(Candidate, Region)
				|| (!Candidate.Regions.empty() && !RegionLess(Candidate.Regions.back(), Region)))
				return Fail(ETerrainWorldOutcome::Corrupt,
					"Terrain World manifest records are not canonically ordered.", OutOutcome, OutError);
			Candidate.Regions.push_back(std::move(Region));
		}
		if (!Records.IsAtEnd())
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain World manifest has trailing record bytes.", OutOutcome, OutError);
		OutManifest = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto CookTerrainWorld(const FTerrainWorldCookRequest& Request,
		FTerrainWorldManifest& OutManifest, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutManifest = {};
		if (!Request.WorldId.IsValid() || Request.CookRoot.empty() || !Request.CookRoot.is_absolute()
			|| Request.VirtualWorldRoot.empty() || Request.VirtualWorldRoot.back() == '/'
			|| Request.PackageTemplateBytes.empty() || Request.TargetPlatform == Asset::ECookTargetPlatform::Invalid
			|| Request.TargetProfile == Asset::ECookTargetProfile::Invalid)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain World Cook request is invalid.", OutOutcome, OutError);
		if (Request.Generations.size() > 262144)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World Cook exceeds the profile tile ceiling.", OutOutcome, OutError);
		FTerrainWorldManifest Manifest{Request.WorldId, TerrainWorldSchemaVersion,
			Request.TargetPlatform, Request.TargetProfile};
		for (const FTerrainTileGeneration& Generation : Request.Generations)
		{
			if (Generation.Tile.WorldId != Request.WorldId || !Generation.GenerationId.IsValid())
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World Cook generation identity is invalid.", OutOutcome, OutError);
			FTerrainRegionKey RegionKey;
			if (!GetTerrainRegionKey(Generation.Tile, RegionKey, OutOutcome, OutError)) return false;
			auto It = std::ranges::find_if(Manifest.Regions,
				[&](const FTerrainManifestRegion& Value) { return Value.Region == RegionKey; });
			if (It == Manifest.Regions.end())
			{
				Manifest.Regions.push_back({RegionKey, IsInstalled(Request.InstalledRegions, RegionKey),
					RegionPath(Request.VirtualWorldRoot, RegionKey), {}});
				It = std::prev(Manifest.Regions.end());
			}
			if (!It->bInstalled) continue;
			for (const FTerrainTileProduct& Product : Generation.Products)
			{
				FTerrainTileProduct Validated;
				if (Product.Tile != Generation.Tile || Product.GenerationId != Generation.GenerationId
					|| !DecodeTerrainTileProduct(Product.Bytes, Product.ProductClass,
						Validated, OutOutcome, OutError))
					return Fail(ETerrainWorldOutcome::Corrupt,
						"Terrain World Cook requires a complete validated generation.", OutOutcome, OutError);
			}
		}
		std::ranges::sort(Manifest.Regions, RegionLess);
		for (size_t Index = 0; Index < Request.InstalledRegions.size(); ++Index)
		{
			const FTerrainRegionKey& Selected = Request.InstalledRegions[Index];
			if (Selected.WorldId != Request.WorldId
				|| Selected.SchemeVersion != TerrainWorldTileSchemeVersion
				|| std::find(Request.InstalledRegions.begin(),
					Request.InstalledRegions.begin() + Index, Selected)
					!= Request.InstalledRegions.begin() + Index
				|| std::ranges::find(Manifest.Regions, Selected, &FTerrainManifestRegion::Region)
					== Manifest.Regions.end())
				return Fail(ETerrainWorldOutcome::MissingDependency,
					"Terrain World Cook selected a missing, duplicate, or incompatible region.",
					OutOutcome, OutError);
		}
		Asset::FCookContext Cook(Request.CookRoot, Request.TargetPlatform, Request.TargetProfile);
		for (FTerrainManifestRegion& Region : Manifest.Regions)
		{
			if (!Region.bInstalled) continue;
			std::vector<Asset::FCookedBulkPayload> Payloads;
			std::vector<const FTerrainTileProduct*> Products;
			for (const FTerrainTileGeneration& Generation : Request.Generations)
			{
				FTerrainRegionKey Key;
				if (!GetTerrainRegionKey(Generation.Tile, Key, OutOutcome, OutError)) return false;
				if (Key != Region.Region) continue;
				for (const FTerrainTileProduct& Product : Generation.Products)
				{
					Payloads.push_back({ProductPayloadId(Product), 1, TerrainWorldSchemaVersion,
						Asset::ECookedPayloadCompression::None, 16, Product.Bytes});
					Products.push_back(&Product);
				}
			}
			if (Products.size() > 64u * 5u)
				return Fail(ETerrainWorldOutcome::BudgetRejected,
					"Terrain World region exceeds 64 products per class.", OutOutcome, OutError);
			uint64 LogicalBytes = 0;
			for (const auto& Payload : Payloads) LogicalBytes += Payload.Bytes.size();
			if (LogicalBytes > TerrainWorldMaximumRegionLogicalBytes)
				return Fail(ETerrainWorldOutcome::BudgetRejected,
					"Terrain World region exceeds its logical byte ceiling.", OutOutcome, OutError);
			if (!Cook.AddPackage(Region.VirtualPackagePath, std::move(Payloads),
				[&, Products](std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
					std::vector<std::byte>& OutBytes, std::string*) {
					if (Descriptors.size() != Products.size()) return false;
					uint64 MaximumStoredEnd = 0;
					for (const Asset::FCookedPayloadDescriptor& Descriptor : Descriptors)
					{
						if (Descriptor.Offset > std::numeric_limits<uint64>::max() - Descriptor.StoredSize)
							return false;
						MaximumStoredEnd = std::max(MaximumStoredEnd,
							Descriptor.Offset + Descriptor.StoredSize);
					}
					if (MaximumStoredEnd > TerrainWorldMaximumRegionStoredBytes) return false;
					for (const FTerrainTileProduct* Product : Products)
					{
						const FGuid Id = ProductPayloadId(*Product);
						const auto Descriptor = std::ranges::find(Descriptors, Id,
							&Asset::FCookedPayloadDescriptor::PayloadId);
						if (Descriptor == Descriptors.end()) return false;
						Region.Products.push_back({Product->Tile, Product->GenerationId,
							Product->ProductClass, *Descriptor, FXxHash128::HashBuffer(Product->Bytes),
							Product->Dependencies});
					}
					std::ranges::sort(Region.Products, ProductLess);
					OutBytes = Request.PackageTemplateBytes;
					return true;
				}, &OutError))
				return Fail(ETerrainWorldOutcome::PublicationFailed,
					"Terrain World region package could not be staged: " + OutError, OutOutcome, OutError);
		}
		std::vector<std::byte> ManifestBytes;
		if (!EncodeTerrainWorldManifest(Manifest, ManifestBytes, OutOutcome, OutError)) return false;
		if (!Cook.AddPackage(Request.VirtualWorldRoot + "/Manifest",
			Request.PackageTemplateBytes,
			{{ManifestPayloadId(Request.WorldId), 1, TerrainWorldSchemaVersion,
				Asset::ECookedPayloadCompression::None, 16, std::move(ManifestBytes)}}, &OutError)
			|| !Cook.Publish(&OutError))
			return Fail(ETerrainWorldOutcome::PublicationFailed,
				"Terrain World Cook publication failed: " + OutError, OutOutcome, OutError);
		OutManifest = std::move(Manifest);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto LoadCookedTerrainWorldManifest(
		const Asset::FAssetRuntimeConfiguration& RuntimeConfiguration,
		std::string_view VirtualWorldRoot, const FTerrainWorldId& WorldId,
		Asset::ECookTargetPlatform ExpectedPlatform, Asset::ECookTargetProfile ExpectedProfile,
		std::shared_ptr<const FTerrainWorldManifest>& OutManifest,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutManifest.reset();
		std::filesystem::path PackagePath, BulkPath;
		const std::string VirtualPath = std::string(VirtualWorldRoot) + "/Manifest";
		if (!RuntimeConfiguration.RequiresCookedPayload()
			|| !Asset::ResolveCookedPackagePath(RuntimeConfiguration.GetCookRoot(), VirtualPath, PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(RuntimeConfiguration.GetCookRoot(), PackagePath, BulkPath, &OutError))
			return Fail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(BulkPath, ExpectedPlatform, ExpectedProfile, Container, &OutError))
			return Fail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		const FGuid PayloadId = ManifestPayloadId(WorldId);
		const auto It = std::ranges::find(Container.Entries, PayloadId,
			&Asset::FCookedPayloadDescriptor::PayloadId);
		if (It == Container.Entries.end())
			return Fail(ETerrainWorldOutcome::MissingDependency,
				"Cooked Terrain World manifest product is missing.", OutOutcome, OutError);
		std::span<const std::byte> Bytes;
		if (!Asset::ResolveCookedPayload(Container, *It, Bytes, &OutError))
			return Fail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		FTerrainWorldManifest Decoded;
		if (!DecodeTerrainWorldManifest(Bytes, WorldId, ExpectedPlatform, ExpectedProfile,
			Decoded, OutOutcome, OutError)) return false;
		OutManifest = std::make_shared<const FTerrainWorldManifest>(std::move(Decoded));
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto LoadCookedTerrainProduct(const Asset::FAssetRuntimeConfiguration& RuntimeConfiguration,
		const std::shared_ptr<const FTerrainWorldManifest>& Manifest,
		const FTerrainTileKey& Tile, const FGuid& GenerationId,
		ETerrainTileProductClass ProductClass, FTerrainCookedProductHandle& OutHandle,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutHandle = {};
		if (!Manifest || Manifest->WorldId != Tile.WorldId)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Cooked Terrain product request does not match its manifest.", OutOutcome, OutError);
		FTerrainRegionKey RegionKey;
		if (!GetTerrainRegionKey(Tile, RegionKey, OutOutcome, OutError)) return false;
		const auto Region = std::ranges::find_if(Manifest->Regions,
			[&](const FTerrainManifestRegion& Value) { return Value.Region == RegionKey; });
		if (Region == Manifest->Regions.end() || !Region->bInstalled)
			return Fail(ETerrainWorldOutcome::Unavailable,
				"Cooked Terrain region is not installed.", OutOutcome, OutError);
		const auto Entry = std::ranges::find_if(Region->Products,
			[&](const FTerrainManifestProductEntry& Value) {
				return Value.Tile == Tile && Value.GenerationId == GenerationId
					&& Value.ProductClass == ProductClass;
			});
		if (Entry == Region->Products.end())
			return Fail(ETerrainWorldOutcome::MissingDependency,
				"Cooked Terrain product is missing.", OutOutcome, OutError);
		auto Payload = std::make_shared<Asset::FCookedPackagePayload>();
		if (!Asset::LoadCookedPackagePayload(RuntimeConfiguration, Region->VirtualPackagePath,
			Entry->Descriptor, Manifest->TargetPlatform, Manifest->TargetProfile,
			*Payload, &OutError))
			return Fail(MapLoadError(OutError), OutError, OutOutcome, OutError);
		if (FXxHash128::HashBuffer(Payload->Payload) != Entry->ProductHash)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Cooked Terrain product manifest checksum is invalid.", OutOutcome, OutError);
		FTerrainTileProduct Product;
		if (!DecodeTerrainTileProduct(Payload->Payload, ProductClass, Product, OutOutcome, OutError)) return false;
		if (Product.Tile != Tile || Product.GenerationId != GenerationId
			|| Product.Dependencies != Entry->Dependencies)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Cooked Terrain product identity or dependencies do not match the manifest.", OutOutcome, OutError);
		OutHandle = {Manifest, std::move(Payload), *Entry};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}
}
