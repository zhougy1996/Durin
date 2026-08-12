#include "Terrain/TerrainHeightmap.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapRenderStateRecreateContext.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin
{
	namespace
	{
		auto SetBoundedDiagnostic(std::string& Target, std::string Message) -> void
		{
			constexpr size_t MaximumDiagnosticBytes = 2048;
			if (Message.size() > MaximumDiagnosticBytes)
				Message.resize(MaximumDiagnosticBytes);
			Target = std::move(Message);
		}

	}

	auto BuildTerrainHeightmapPayload(
		uint32 Width,
		uint32 Height,
		std::span<const uint16> Samples,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool
	{
		OutPayload.reset();
		OutError.clear();
		const uint64 SampleCount = static_cast<uint64>(Width) * Height;
		if (Width < 2 || Height < 2 || Width > MaximumTerrainHeightmapDimension
			|| Height > MaximumTerrainHeightmapDimension
			|| SampleCount > MaximumTerrainHeightmapSamples
			|| SampleCount != Samples.size())
		{
			OutError = "Terrain heightmap dimensions or sample count violate the frozen limits.";
			return false;
		}

		uint32 LevelWidth = (Width + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		uint32 LevelHeight = (Height + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		uint64 NodeCount = 0;
		uint32 LevelCount = 0;
		while (true)
		{
			const uint64 LevelNodes = static_cast<uint64>(LevelWidth) * LevelHeight;
			if (LevelNodes > std::numeric_limits<uint64>::max() - NodeCount)
			{
				OutError = "Terrain heightmap hierarchy node count overflowed.";
				return false;
			}
			NodeCount += LevelNodes;
			++LevelCount;
			if (LevelWidth == 1 && LevelHeight == 1) break;
			LevelWidth = (LevelWidth + 1) / 2;
			LevelHeight = (LevelHeight + 1) / 2;
		}
		if (NodeCount * sizeof(FTerrainHeightmapMinMaxNode)
			> MaximumTerrainHeightmapHierarchyBytes)
		{
			OutError = "Terrain heightmap hierarchy exceeds the frozen byte ceiling.";
			return false;
		}

		auto Candidate = std::make_shared<FTerrainHeightmapPayload>();
		Candidate->Width = Width;
		Candidate->Height = Height;
		Candidate->Samples.assign(Samples.begin(), Samples.end());
		Candidate->Nodes.reserve(static_cast<size_t>(NodeCount));
		Candidate->Levels.reserve(LevelCount);
		const auto Global = std::minmax_element(Samples.begin(), Samples.end());
		Candidate->Minimum = *Global.first;
		Candidate->Maximum = *Global.second;

		LevelWidth = (Width + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		LevelHeight = (Height + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		Candidate->Levels.push_back({
			.Width = LevelWidth,
			.Height = LevelHeight,
			.NodeOffset = 0,
			.SampleRegionSize = TerrainHeightmapBaseRegionSize});
		for (uint32 RegionY = 0; RegionY < LevelHeight; ++RegionY)
			for (uint32 RegionX = 0; RegionX < LevelWidth; ++RegionX)
			{
				const uint32 MinX = RegionX * TerrainHeightmapBaseRegionSize;
				const uint32 MinY = RegionY * TerrainHeightmapBaseRegionSize;
				const uint32 MaxX = std::min(MinX + TerrainHeightmapBaseRegionSize, Width);
				const uint32 MaxY = std::min(MinY + TerrainHeightmapBaseRegionSize, Height);
				uint16 NodeMinimum = std::numeric_limits<uint16>::max();
				uint16 NodeMaximum = std::numeric_limits<uint16>::min();
				for (uint32 Y = MinY; Y < MaxY; ++Y)
					for (uint32 X = MinX; X < MaxX; ++X)
					{
						const uint16 Value = Samples[static_cast<uint64>(Y) * Width + X];
						NodeMinimum = std::min(NodeMinimum, Value);
						NodeMaximum = std::max(NodeMaximum, Value);
					}
				Candidate->Nodes.push_back({NodeMinimum, NodeMaximum});
			}

		while (LevelWidth != 1 || LevelHeight != 1)
		{
			const FTerrainHeightmapLevel& Previous = Candidate->Levels.back();
			const uint32 NextWidth = (LevelWidth + 1) / 2;
			const uint32 NextHeight = (LevelHeight + 1) / 2;
			const uint64 NextOffset = Candidate->Nodes.size();
			Candidate->Levels.push_back({
				.Width = NextWidth,
				.Height = NextHeight,
				.NodeOffset = NextOffset,
				.SampleRegionSize = Previous.SampleRegionSize * 2});
			for (uint32 Y = 0; Y < NextHeight; ++Y)
				for (uint32 X = 0; X < NextWidth; ++X)
				{
					uint16 NodeMinimum = std::numeric_limits<uint16>::max();
					uint16 NodeMaximum = std::numeric_limits<uint16>::min();
					for (uint32 ChildY = Y * 2; ChildY < std::min(Y * 2 + 2, LevelHeight); ++ChildY)
						for (uint32 ChildX = X * 2; ChildX < std::min(X * 2 + 2, LevelWidth); ++ChildX)
						{
							const auto& Node = Candidate->Nodes[static_cast<size_t>(
								Previous.NodeOffset + static_cast<uint64>(ChildY) * LevelWidth + ChildX)];
							NodeMinimum = std::min(NodeMinimum, Node.Minimum);
							NodeMaximum = std::max(NodeMaximum, Node.Maximum);
						}
					Candidate->Nodes.push_back({NodeMinimum, NodeMaximum});
				}
			LevelWidth = NextWidth;
			LevelHeight = NextHeight;
		}
		OutPayload = std::move(Candidate);
		return true;
	}

	auto FTerrainHeightmapPayload::IsValid() const -> bool
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Canonical;
		std::string Error;
		if (!BuildTerrainHeightmapPayload(Width, Height, Samples, Canonical, Error)) return false;
		return Minimum == Canonical->Minimum && Maximum == Canonical->Maximum
			&& Levels == Canonical->Levels && Nodes == Canonical->Nodes;
	}

	auto FTerrainHeightmapPayload::GetSample(
		uint32 X, uint32 Y, uint16& OutSample) const -> bool
	{
		if (X >= Width || Y >= Height) return false;
		OutSample = Samples[static_cast<uint64>(Y) * Width + X];
		return true;
	}

	auto FTerrainHeightmapPayload::QueryMinMax(
		uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY,
		uint16& OutMinimum, uint16& OutMaximum) const -> bool
	{
		if (MinX >= MaxX || MinY >= MaxY || MaxX > Width || MaxY > Height) return false;
		OutMinimum = std::numeric_limits<uint16>::max();
		OutMaximum = std::numeric_limits<uint16>::min();
		const FTerrainHeightmapLevel& Base = Levels.front();
		const uint32 FirstRegionX = MinX / TerrainHeightmapBaseRegionSize;
		const uint32 FirstRegionY = MinY / TerrainHeightmapBaseRegionSize;
		const uint32 LastRegionX = (MaxX - 1) / TerrainHeightmapBaseRegionSize;
		const uint32 LastRegionY = (MaxY - 1) / TerrainHeightmapBaseRegionSize;
		for (uint32 RegionY = FirstRegionY; RegionY <= LastRegionY; ++RegionY)
			for (uint32 RegionX = FirstRegionX; RegionX <= LastRegionX; ++RegionX)
			{
				const uint32 RegionMinX = RegionX * TerrainHeightmapBaseRegionSize;
				const uint32 RegionMinY = RegionY * TerrainHeightmapBaseRegionSize;
				const uint32 RegionMaxX = std::min(RegionMinX + TerrainHeightmapBaseRegionSize, Width);
				const uint32 RegionMaxY = std::min(RegionMinY + TerrainHeightmapBaseRegionSize, Height);
				if (MinX <= RegionMinX && MinY <= RegionMinY
					&& MaxX >= RegionMaxX && MaxY >= RegionMaxY)
				{
					const auto& Node = Nodes[static_cast<size_t>(
						Base.NodeOffset + static_cast<uint64>(RegionY) * Base.Width + RegionX)];
					OutMinimum = std::min(OutMinimum, Node.Minimum);
					OutMaximum = std::max(OutMaximum, Node.Maximum);
					continue;
				}
				for (uint32 Y = std::max(MinY, RegionMinY); Y < std::min(MaxY, RegionMaxY); ++Y)
					for (uint32 X = std::max(MinX, RegionMinX); X < std::min(MaxX, RegionMaxX); ++X)
					{
						const uint16 Value = Samples[static_cast<uint64>(Y) * Width + X];
						OutMinimum = std::min(OutMinimum, Value);
						OutMaximum = std::max(OutMaximum, Value);
					}
			}
		return true;
	}

	auto FTerrainHeightmapPayload::GetSampleBytes() const -> uint64
	{
		return static_cast<uint64>(Samples.size()) * sizeof(uint16);
	}

	auto FTerrainHeightmapPayload::GetHierarchyBytes() const -> uint64
	{
		return static_cast<uint64>(Nodes.size()) * sizeof(FTerrainHeightmapMinMaxNode)
			+ static_cast<uint64>(Levels.size()) * sizeof(FTerrainHeightmapLevel);
	}

	auto FTerrainHeightmapPayload::GetRetainedBytes() const -> uint64
	{
		return GetSampleBytes() + GetHierarchyBytes();
	}

	DTerrainHeightmap::DTerrainHeightmap(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	DTerrainHeightmap::~DTerrainHeightmap() = default;

	auto DTerrainHeightmap::Serialize(FArchive& Ar) -> void
	{
		Super::Serialize(Ar);
		if (Ar.GetPurpose() != EArchivePurpose::Duplicate) return;
		uint32 DuplicateWidth = Ar.IsSaving() && Payload ? Payload->Width : 0;
		uint32 DuplicateHeight = Ar.IsSaving() && Payload ? Payload->Height : 0;
		std::vector<uint16> DuplicateSamples = Ar.IsSaving() && Payload
			? Payload->Samples : std::vector<uint16>{};
		uint64 DuplicateSampleCount = DuplicateSamples.size();
		Ar << DuplicateWidth << DuplicateHeight << DuplicateSampleCount;
		if (Ar.IsLoading() && !Ar.HasError())
		{
			if (DuplicateSampleCount > MaximumTerrainHeightmapSamples)
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Duplicated terrain heightmap exceeds the sample ceiling.");
			else DuplicateSamples.resize(static_cast<size_t>(DuplicateSampleCount));
		}
		for (uint16& Sample : DuplicateSamples) Ar << Sample;
		if (Ar.IsLoading() && !Ar.HasError())
		{
			std::shared_ptr<const FTerrainHeightmapPayload> DuplicatePayload;
			std::string Error;
			if (!BuildTerrainHeightmapPayload(
				DuplicateWidth, DuplicateHeight, DuplicateSamples, DuplicatePayload, Error))
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
			else PublishPayload(std::move(DuplicatePayload), false);
		}
	}

	auto DTerrainHeightmap::BeginDestroy() -> void
	{
		Payload.reset();
		Status = ETerrainHeightmapStatus::Unavailable;
		Super::BeginDestroy();
	}

	auto DTerrainHeightmap::PublishPayload(
		std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
		bool bAdvanceRevision) -> void
	{
		FTerrainHeightmapRenderStateRecreateContext RecreateContext(this);
		Payload = std::move(InPayload);
		Width = Payload->Width;
		Height = Payload->Height;
		Minimum = Payload->Minimum;
		Maximum = Payload->Maximum;
		SampleBytes = Payload->GetSampleBytes();
		HierarchyBytes = Payload->GetHierarchyBytes();
		RetainedBytes = Payload->GetRetainedBytes();
		if (bAdvanceRevision) ++Revision;
		Status = ETerrainHeightmapStatus::Ready;
	}

	auto DTerrainHeightmap::GetSample(uint32 X, uint32 Y, uint16& OutSample) const -> bool
	{
		return Payload && Payload->GetSample(X, Y, OutSample);
	}

	auto DTerrainHeightmap::QueryMinMax(
		uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY,
		uint16& OutMinimum, uint16& OutMaximum) const -> bool
	{
		return Payload && Payload->QueryMinMax(
			MinX, MinY, MaxX, MaxY, OutMinimum, OutMaximum);
	}

	auto DTerrainHeightmap::PublishAuthoringCandidate(
		FTerrainHeightmapSourceImportData InSourceImportData,
		uint64 InSourceFileSize,
		int64 InSourceLastWriteTime,
		std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
		std::string InDerivedDataKey,
		std::string InDiagnostic,
		bool bAdvanceRevision,
		bool bMarkPackageDirty,
		bool bInLoadedFromDerivedDataCache) -> void
	{
		SourceImportData = std::move(InSourceImportData);
		SourceFileSize = InSourceFileSize;
		SourceLastWriteTime = InSourceLastWriteTime;
		SourceBitDepth = 16;
		SourceChannelCount = 1;
		DerivedDataKey = std::move(InDerivedDataKey);
		bLoadedFromDerivedDataCache = bInLoadedFromDerivedDataCache;
		PublishPayload(std::move(InPayload), bAdvanceRevision);
		SetBoundedDiagnostic(LastDiagnostic, std::move(InDiagnostic));
		if (bMarkPackageDirty) MarkPackageDirty();
	}

	auto DTerrainHeightmap::InitializeFromSamples(
		uint32 InWidth,
		uint32 InHeight,
		std::span<const uint16> InSamples,
		std::string& OutError) -> bool
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		if (!BuildTerrainHeightmapPayload(InWidth, InHeight, InSamples, Candidate, OutError))
		{
			Status = ETerrainHeightmapStatus::Failed;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		PublishPayload(std::move(Candidate), true);
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		LastDiagnostic = "Built canonical terrain heightmap payload from exact samples.";
		MarkPackageDirty();
		return true;
	}

	auto DTerrainHeightmap::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPayload(OutError);
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}
		if (!GetPackage() && Payload && Payload->IsValid())
		{
			OutError.clear();
			return true;
		}
		return InvokeTerrainHeightmapUncookedPostLoadHandler(*this, OutError);
	}

	auto DTerrainHeightmap::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			OutError = std::format("Cooked terrain heightmap '{}': {}", GetObjectPath(), Message);
			Status = ETerrainHeightmapStatus::Failed;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		};
		if (CookedPayload.PayloadId != TerrainHeightmapPrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != TerrainHeightmapPayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required THPL descriptor is missing or incompatible.");
		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage() || !Asset::ResolveCookedPackagePath(
			LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
			return FailCooked(OutError);
		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Container, &OutError))
			return FailCooked(OutError);
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		auto MutableCandidate = std::make_shared<FTerrainHeightmapPayload>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		MutableCandidate->Serialize(PayloadAr, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game);
		if (PayloadAr.HasError()) return FailCooked(PayloadAr.GetFailure()->Message);
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate = std::move(MutableCandidate);
		if (Candidate->Width != Width || Candidate->Height != Height
			|| Candidate->Minimum != Minimum || Candidate->Maximum != Maximum)
			return FailCooked("package facts do not match the required THPL payload.");
		PublishPayload(std::move(Candidate), false);
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		LastDiagnostic = "Loaded validated cooked terrain heightmap payload.";
		OutError.clear();
		return true;
	}

	auto DTerrainHeightmap::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game
			|| !Payload || !Payload->IsValid())
		{
			OutError = std::format(
				"Terrain heightmap '{}' is not ready for a Win64 game cook.", GetObjectPath());
			return false;
		}
		std::vector<uint8> PayloadBytes;
		FCanonicalMemoryWriter CookAr(PayloadBytes, EArchivePurpose::CookedPayload);
		const_cast<FTerrainHeightmapPayload&>(*Payload).Serialize(
			CookAr, Context.GetTargetPlatform(), Context.GetTargetProfile());
		if (CookAr.HasError())
		{
			OutError = CookAr.GetFailure()->Message;
			return false;
		}
		Asset::FCookedBulkPayload Bulk{
			.PayloadId = TerrainHeightmapPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TerrainHeightmapPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TerrainHeightmapPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(Bulk)},
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != TerrainHeightmapPrimaryCookedPayloadId)
				{
					if (Error) *Error = "Terrain heightmap cook did not produce its required descriptor.";
					return false;
				}
				const FTerrainHeightmapSourceImportData SavedSource = SourceImportData;
				const uint64 SavedSize = SourceFileSize;
				const int64 SavedTime = SourceLastWriteTime;
				const uint32 SavedBits = SourceBitDepth;
				const uint32 SavedChannels = SourceChannelCount;
				const Asset::FCookedPayloadDescriptor SavedCooked = CookedPayload;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceImportData = {};
					SourceFileSize = 0;
					SourceLastWriteTime = 0;
					SourceBitDepth = 0;
					SourceChannelCount = 0;
				}
				Asset::FAssetPackageSerializationOptions Options;
				if (!bRetainDiagnosticSourceMetadata)
					Options.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						if (Object != this) return true;
						const FName Name = Property->NamePrivate;
						return Name != FName("SourceImportData")
							&& Name != FName("SourceFileSize")
							&& Name != FName("SourceLastWriteTime")
							&& Name != FName("SourceBitDepth")
							&& Name != FName("SourceChannelCount");
					};
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, Options);
				SourceImportData = SavedSource;
				SourceFileSize = SavedSize;
				SourceLastWriteTime = SavedTime;
				SourceBitDepth = SavedBits;
				SourceChannelCount = SavedChannels;
				CookedPayload = SavedCooked;
				if (!Result && Error) *Error = Result.Message;
				return static_cast<bool>(Result);
			}, &OutError);
	}

	auto DTerrainHeightmap::IsSemanticImportNoOp(
		const DTerrainHeightmap& Candidate) const -> bool
	{
		return SourceImportData == Candidate.SourceImportData
			&& DerivedDataKey == Candidate.DerivedDataKey
			&& Payload && Candidate.Payload
			&& Payload->Samples == Candidate.Payload->Samples;
	}

	auto DTerrainHeightmap::PrepareCandidateRevision(DTerrainHeightmap& Candidate) const -> void
	{
		Candidate.Revision = Revision + (Payload && Candidate.Payload
			&& Payload->Samples == Candidate.Payload->Samples ? 0 : 1);
		if (Candidate.Revision == 0) Candidate.Revision = 1;
	}

	auto DTerrainHeightmap::ExchangeImportedState(DTerrainHeightmap& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(SourceFileSize, Other.SourceFileSize);
		std::swap(SourceLastWriteTime, Other.SourceLastWriteTime);
		std::swap(SourceBitDepth, Other.SourceBitDepth);
		std::swap(SourceChannelCount, Other.SourceChannelCount);
		std::swap(Width, Other.Width);
		std::swap(Height, Other.Height);
		std::swap(Minimum, Other.Minimum);
		std::swap(Maximum, Other.Maximum);
		std::swap(Revision, Other.Revision);
		std::swap(SampleBytes, Other.SampleBytes);
		std::swap(HierarchyBytes, Other.HierarchyBytes);
		std::swap(RetainedBytes, Other.RetainedBytes);
		std::swap(CookedPayload, Other.CookedPayload);
		std::swap(Status, Other.Status);
		std::swap(DerivedDataKey, Other.DerivedDataKey);
		std::swap(LastDiagnostic, Other.LastDiagnostic);
		std::swap(Payload, Other.Payload);
		std::swap(bLoadedFromDerivedDataCache, Other.bLoadedFromDerivedDataCache);
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

}
