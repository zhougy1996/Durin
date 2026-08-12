#include "Terrain/TerrainHeightmap.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapRenderStateRecreateContext.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view HeightmapDecoderId = "DurinHeightmapPng";
		constexpr uint32 HeightmapDecoderVersion = 1;
		constexpr std::string_view DefaultHeightmapSourceRoot = "TerrainHeightmaps";
		constexpr uint64 HeightmapDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;

		auto GetHeightmapObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore(
				"TerrainHeightmap/Objects", MaximumTerrainHeightmapPayloadBytes);
		}

		auto SetBoundedDiagnostic(std::string& Target, std::string Message) -> void
		{
			constexpr size_t MaximumDiagnosticBytes = 2048;
			if (Message.size() > MaximumDiagnosticBytes)
				Message.resize(MaximumDiagnosticBytes);
			Target = std::move(Message);
		}

		auto MakeDerivedDataKey(
			const FTerrainHeightmapSourceImportData& Source,
			std::string& OutKey,
			std::string& OutError) -> bool
		{
			if (!Source.HasContentHash())
			{
				OutError = "Terrain heightmap source content identity is missing.";
				return false;
			}
			return BuildTerrainHeightmapDerivedDataKey({
				.SourceContentHash = {
					.HashLow = Source.SourceContentHashLow,
					.HashHigh = Source.SourceContentHashHigh},
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game}, OutKey, OutError);
		}

		auto StoreDerivedData(
			std::string_view Key,
			const FTerrainHeightmapPayload& Payload,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!EncodeTerrainHeightmapPayload(
				Payload, Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game, Bytes, OutError)
				|| !GetHeightmapObjectStore().Write(Key, Bytes, &OutError)) return false;
			const Asset::FDerivedDataObjectCleanupResult Cleanup =
				GetHeightmapObjectStore().CleanupToBudget(HeightmapDerivedDataBudgetBytes, 16);
			if (!Cleanup.Message.empty())
				DURIN_WARN("Terrain heightmap DDC cleanup: {}", Cleanup.Message);
			return true;
		}

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view RequestedSourcePath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format(
					"Terrain heightmap asset {} is outside a registered mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path Relative = RequestedSourcePath.empty()
				? std::filesystem::path(DefaultHeightmapSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + ".png")
				: std::filesystem::path(RequestedSourcePath);
			if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested =
					PathUtilities::ResolveSourcePath(
						RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested || Requested.Mount != Mount)
				{
					OutError = Requested ? "Heightmap source must remain in the asset mount."
						: Requested.Message;
					return false;
				}
				Relative = Requested.RelativePath;
			}
			Relative = Relative.lexically_normal();
			const std::string RelativeText = Relative.generic_string();
			std::string Extension = Relative.extension().generic_string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			if (Relative.empty() || Relative.is_absolute() || RelativeText == ".."
				|| RelativeText.starts_with("../") || Extension != ".png")
			{
				OutError = "Heightmap source destination must be a normalized mount-relative .png path.";
				return false;
			}
			OutStoredPath = Mount->VirtualRoot + RelativeText;
			return true;
		}

		auto ResolveHeightmapSource(
			const DTerrainHeightmap& Heightmap,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const FTerrainHeightmapSourceImportData& Source = Heightmap.GetSourceImportData();
			if (!Source.HasSource() || Source.DecoderId != HeightmapDecoderId
				|| Source.DecoderVersion != HeightmapDecoderVersion || !Heightmap.GetPackage())
			{
				OutError = "Terrain heightmap source provenance is missing or incompatible.";
				return false;
			}
			const PathUtilities::FMountPolicyResult Dependency =
				PathUtilities::CheckMountDependency(
					Heightmap.GetPackage()->GetPackagePath(), Source.SourcePath.Path);
			if (!Dependency)
			{
				OutError = Dependency.Message;
				return false;
			}
			const PathUtilities::FSourcePathResult Resolved = PathUtilities::ResolveSourcePath(
				Source.SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPath = Resolved.PhysicalPath;
			return true;
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
		std::string InDiagnostic) -> void
	{
		SourceImportData = std::move(InSourceImportData);
		SourceFileSize = InSourceFileSize;
		SourceLastWriteTime = InSourceLastWriteTime;
		SourceBitDepth = 16;
		SourceChannelCount = 1;
		DerivedDataKey = std::move(InDerivedDataKey);
		bLoadedFromDerivedDataCache = false;
		PublishPayload(std::move(InPayload), true);
		SetBoundedDiagnostic(LastDiagnostic, std::move(InDiagnostic));
		MarkPackageDirty();
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

	auto DTerrainHeightmap::BuildFromEncodedBytes(
		std::span<const uint8> EncodedBytes,
		const FSourcePath& InSourcePath,
		std::string& OutError) -> bool
	{
#if DURIN_WITH_EDITOR
		if (!GetPackage())
		{
			OutError = "Terrain heightmap candidates require an owning package.";
			return false;
		}
		const PathUtilities::FSourcePathResult Resolved = PathUtilities::ResolveSourcePath(
			InSourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency = PathUtilities::CheckMountDependency(
			GetPackage()->GetPackagePath(), Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}

		Asset::FDecodedGrayscale16Image Decoded;
		if (!Asset::DecodeGrayscale16PngFromMemory(
			EncodedBytes, Decoded, OutError,
			{.MaximumEncodedBytes = MaximumTerrainHeightmapEncodedBytes,
				.MaximumDecodedPixels = MaximumTerrainHeightmapSamples}))
		{
			Status = ETerrainHeightmapStatus::Failed;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		if (!BuildTerrainHeightmapPayload(
			Decoded.Width, Decoded.Height, Decoded.Samples, Candidate, OutError))
		{
			Status = ETerrainHeightmapStatus::Failed;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		const FXxHash128 SourceHash = FXxHash128::HashBuffer(EncodedBytes);
		FTerrainHeightmapSourceImportData CandidateSource{
			.SourcePath = {.Path = Resolved.NormalizedVirtualPath},
			.SourceContentHashLow = SourceHash.HashLow,
			.SourceContentHashHigh = SourceHash.HashHigh,
			.DecoderId = std::string(HeightmapDecoderId),
			.DecoderVersion = HeightmapDecoderVersion};
		std::string CandidateKey;
		if (!MakeDerivedDataKey(CandidateSource, CandidateKey, OutError)
			|| !StoreDerivedData(CandidateKey, *Candidate, OutError))
		{
			Status = ETerrainHeightmapStatus::Failed;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		SourceImportData = std::move(CandidateSource);
		SourceFileSize = EncodedBytes.size();
		SourceLastWriteTime = 0;
		SourceBitDepth = 16;
		SourceChannelCount = 1;
		DerivedDataKey = std::move(CandidateKey);
		bLoadedFromDerivedDataCache = false;
		PublishPayload(std::move(Candidate), true);
		LastDiagnostic = std::format(
			"Decoded {}x{} grayscale16 PNG; samples={} bytes, hierarchy={} bytes, retained={} bytes.",
			Width, Height, SampleBytes, HierarchyBytes, RetainedBytes);
		MarkPackageDirty();
		OutError.clear();
		return true;
#else
		(void)EncodedBytes;
		(void)InSourcePath;
		OutError = "Terrain heightmap source decoding is unavailable in runtime-only builds.";
		return false;
#endif
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
		Payload.reset();
		Status = ETerrainHeightmapStatus::Unavailable;
		bLoadedFromDerivedDataCache = false;
		if (!MakeDerivedDataKey(SourceImportData, DerivedDataKey, OutError))
		{
			Status = ETerrainHeightmapStatus::SourceUnavailable;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		std::vector<uint8> CachedBytes;
		const Asset::FDerivedDataObjectReadResult Read =
			GetHeightmapObjectStore().Read(DerivedDataKey, CachedBytes);
		if (Read)
		{
			std::shared_ptr<const FTerrainHeightmapPayload> CachedPayload;
			const FPayloadDecodeResult Decoded = DecodeTerrainHeightmapPayload(
				CachedBytes, Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game, CachedPayload);
			if (Decoded)
			{
				PublishPayload(std::move(CachedPayload), false);
				bLoadedFromDerivedDataCache = true;
				LastDiagnostic = std::format("Terrain heightmap DDC hit for key {}.", DerivedDataKey);
				OutError.clear();
				return true;
			}
		}

		std::filesystem::path SourcePath;
		if (!ResolveHeightmapSource(*this, SourcePath, OutError)
			|| !std::filesystem::is_regular_file(SourcePath))
		{
			if (OutError.empty()) OutError = std::format(
				"Terrain heightmap source file does not exist: {}.", GetSourceFile());
			Status = ETerrainHeightmapStatus::SourceUnavailable;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(EncodedBytes, SourcePath.generic_string()))
		{
			OutError = std::format("Failed to read terrain heightmap source {}.", SourcePath.generic_string());
			Status = ETerrainHeightmapStatus::SourceUnavailable;
			SetBoundedDiagnostic(LastDiagnostic, OutError);
			return false;
		}
		const uint64 SavedRevision = Revision;
		if (!BuildFromEncodedBytes(EncodedBytes, SourceImportData.SourcePath, OutError)) return false;
		Revision = SavedRevision;
		GetPackage()->ClearDirty();
		LastDiagnostic = std::format("Rebuilt terrain heightmap DDC key {} from source.", DerivedDataKey);
		return true;
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
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		const FPayloadDecodeResult Decoded = DecodeTerrainHeightmapPayload(
			Bytes, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Candidate);
		if (!Decoded) return FailCooked(Decoded.Message);
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
		if (!EncodeTerrainHeightmapPayload(
			*Payload, Context.GetTargetPlatform(), Context.GetTargetProfile(),
			PayloadBytes, OutError)) return false;
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

	auto DTerrainHeightmap::ChangeSourceReference(
		std::string_view SourceVirtualPath, std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Terrain heightmap source changes require an owning package.";
			return false;
		}
		FMountedSourceFile Source;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError)) return false;
		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(EncodedBytes, Source.PhysicalPath.generic_string()))
		{
			OutError = "Failed to read the replacement terrain heightmap source.";
			return false;
		}
		return BuildFromEncodedBytes(EncodedBytes, Source.SourcePath, OutError);
	}

	auto DTerrainHeightmap::ReimportSource(
		std::string_view FilePath, std::string& OutError) -> bool
	{
		std::filesystem::path SourcePath;
		if (!ResolveHeightmapSource(*this, SourcePath, OutError)) return false;
		if (!FilePath.empty())
		{
			std::error_code Error;
			if (std::filesystem::absolute(FilePath, Error).lexically_normal()
				!= std::filesystem::absolute(SourcePath, Error).lexically_normal())
			{
				OutError = "Terrain heightmap reimport may only read its persisted mounted source.";
				return false;
			}
		}
		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(EncodedBytes, SourcePath.generic_string()))
		{
			OutError = "Failed to read the terrain heightmap source for reimport.";
			return false;
		}
		const FXxHash128 Hash = FXxHash128::HashBuffer(EncodedBytes);
		if (Hash.HashLow == SourceImportData.SourceContentHashLow
			&& Hash.HashHigh == SourceImportData.SourceContentHashHigh)
		{
			OutError.clear();
			return true;
		}
		return BuildFromEncodedBytes(EncodedBytes, SourceImportData.SourcePath, OutError);
	}

}
