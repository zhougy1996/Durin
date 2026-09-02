#include "Terrain/TerrainHeightmap.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/Property.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapRenderStateRecreateContext.h"
#include "Terrain/TerrainHeightmapAssetBuild.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto FTerrainHeightmapImportedData::IsValid() const -> bool
	{
		const uint64 Count = static_cast<uint64>(Width) * Height;
		return SchemaVersion == TerrainHeightmapImportedDataSchemaVersion
			&& Width >= 2 && Height >= 2
			&& Width <= MaximumTerrainHeightmapDimension
			&& Height <= MaximumTerrainHeightmapDimension
			&& Count <= MaximumTerrainHeightmapSamples
			&& Samples.GetPayloadSize() == Count * sizeof(uint16);
	}

	auto FTerrainHeightmapImportedData::SetSamples(uint32 InWidth, uint32 InHeight,
		std::span<const uint16> InSamples) -> bool
	{
		const uint64 Count = static_cast<uint64>(InWidth) * InHeight;
		if (InWidth < 2 || InHeight < 2
			|| InWidth > MaximumTerrainHeightmapDimension
			|| InHeight > MaximumTerrainHeightmapDimension
			|| Count > MaximumTerrainHeightmapSamples || Count != InSamples.size())
			return false;
		const std::span<const std::byte> Bytes = std::as_bytes(InSamples);
		if (!Samples.UpdatePayload(Bytes))
			return false;
		Width = InWidth;
		Height = InHeight;
		SchemaVersion = TerrainHeightmapImportedDataSchemaVersion;
		return IsValid();
	}

	auto FTerrainHeightmapImportedData::GetSamples() const -> std::vector<uint16>
	{
		if (!IsValid()) return {};
		const FSharedByteBuffer Payload = Samples.GetPayload().Wait().Buffer;
		const std::span<const std::byte> Bytes = Payload.GetBytes();
		std::vector<uint16> Result(Bytes.size() / sizeof(uint16));
		std::memcpy(Result.data(), Bytes.data(), Bytes.size());
		return Result;
	}

	auto FTerrainHeightmapImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(Width);
		Builder.UpdateValue(Height);
		Builder.UpdateValue(Samples.GetPayloadId());
		return Builder.Finalize();
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

	auto FTerrainHeightmapPayload::HasValidLayout() const -> bool
	{
		const uint64 SampleCount = static_cast<uint64>(Width) * Height;
		if (Width < 2 || Height < 2 || Width > MaximumTerrainHeightmapDimension
			|| Height > MaximumTerrainHeightmapDimension
			|| SampleCount > MaximumTerrainHeightmapSamples
			|| SampleCount != Samples.size() || Levels.empty() || Nodes.empty()
			|| Minimum > Maximum) return false;

		uint32 LevelWidth = (Width + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		uint32 LevelHeight = (Height + TerrainHeightmapBaseRegionSize - 1)
			/ TerrainHeightmapBaseRegionSize;
		uint32 RegionSize = TerrainHeightmapBaseRegionSize;
		uint64 NodeOffset = 0;
		for (size_t LevelIndex = 0; LevelIndex < Levels.size(); ++LevelIndex)
		{
			const FTerrainHeightmapLevel& Level = Levels[LevelIndex];
			if (Level.Width != LevelWidth || Level.Height != LevelHeight
				|| Level.NodeOffset != NodeOffset || Level.SampleRegionSize != RegionSize)
				return false;
			const uint64 LevelNodes = static_cast<uint64>(LevelWidth) * LevelHeight;
			if (NodeOffset > Nodes.size() || LevelNodes > Nodes.size() - NodeOffset) return false;
			NodeOffset += LevelNodes;
			if (LevelWidth == 1 && LevelHeight == 1)
			{
				if (LevelIndex + 1 != Levels.size() || NodeOffset != Nodes.size()) return false;
				const FTerrainHeightmapMinMaxNode& Root = Nodes.back();
				return Root.Minimum == Minimum && Root.Maximum == Maximum;
			}
			LevelWidth = (LevelWidth + 1) / 2;
			LevelHeight = (LevelHeight + 1) / 2;
			if (RegionSize > std::numeric_limits<uint32>::max() / 2) return false;
			RegionSize *= 2;
		}
		return false;
	}

	auto FTerrainHeightmapPayload::IsValid() const -> bool
	{
		if (!HasValidLayout()) return false;
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

	auto DTerrainHeightmap::SetPayload(
		std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
		std::string& OutError,
		bool bAdvanceRevision,
		std::optional<FTerrainHeightmapImportedData> InImportedData) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!InPayload || !InPayload->IsValid())
		{
			OutError = "Terrain heightmap payload is invalid.";
			return false;
		}
		FTerrainHeightmapImportedData Candidate = InImportedData.value_or(
			FTerrainHeightmapImportedData{});
		if ((!InImportedData && !Candidate.SetSamples(
			InPayload->Width, InPayload->Height, InPayload->Samples))
			|| !Candidate.IsValid() || Candidate.Width != InPayload->Width
			|| Candidate.Height != InPayload->Height)
		{
			OutError = "Terrain heightmap canonical imported metadata is incompatible.";
			return false;
		}
		ImportedData = std::move(Candidate);
		CookedPlatformData = {};
		SourceBitDepth = 16;
		SourceChannelCount = 1;
		PublishPayload(std::move(InPayload), bAdvanceRevision);
		OutError.clear();
		return true;
	}

	auto DTerrainHeightmap::InitializeFromSamples(
		uint32 InWidth,
		uint32 InHeight,
		std::span<const uint16> InSamples,
		std::string& OutError) -> bool
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		if (!BuildTerrainHeightmapPayload(InWidth, InHeight, InSamples, Candidate, OutError)
			|| !SetPayload(std::move(Candidate), OutError)) return false;
		MarkPackageDirty();
		return true;
	}

	auto DTerrainHeightmap::PostLoad(std::string& OutError) -> bool
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked terrain heightmap '{}': required PlatformData field is missing.",
					GetObjectPath());
				Status = ETerrainHeightmapStatus::Failed;

				return false;
			}
			Payload.reset();

			Status = ETerrainHeightmapStatus::Ready;

			OutError.clear();
			return true;
		}
		if (!GetPackage() && Payload && Payload->IsValid())
		{
			OutError.clear();
			return true;
		}
		return PrepareTerrainHeightmapPayload(*this, OutError);
	}

	auto DTerrainHeightmap::GetPayload() const
		-> std::shared_ptr<const FTerrainHeightmapPayload>
	{
		if (!Payload && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DTerrainHeightmap*>(this)->LoadCookedPayload(Error);
		}
		return Payload;
	}

	auto DTerrainHeightmap::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"TerrainHeightmap cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* FieldValue = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!Payload || !Payload->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"TerrainHeightmap cooked platform data is unavailable.");
				return;
			}
			FByteArray Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			const_cast<FTerrainHeightmapPayload&>(*Payload).Serialize(
				Writer, ECookTargetPlatform::Win64,
				ECookTargetProfile::Game);
			std::string Error;
			if (Writer.HasError()
				|| !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Error.empty() ? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DTerrainHeightmap"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DTerrainHeightmap::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			OutError = std::format("Cooked terrain heightmap '{}': {}", GetObjectPath(), Message);
			Status = ETerrainHeightmapStatus::Failed;

			return false;
		};
		std::span<const std::byte> Bytes;
		if (!CookedPlatformData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		auto MutableCandidate = std::make_shared<FTerrainHeightmapPayload>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		MutableCandidate->Serialize(PayloadAr, ECookTargetPlatform::Win64,
			ECookTargetProfile::Game);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
		{
			const std::string Error(PayloadAr.GetError());
			CookedPlatformData.UnlockReadOnly();
			return FailCooked(Error);
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate = std::move(MutableCandidate);
		if (Candidate->Width != Width || Candidate->Height != Height
			|| Candidate->Minimum != Minimum || Candidate->Maximum != Maximum)
		{
			CookedPlatformData.UnlockReadOnly();
			return FailCooked("package facts do not match the PlatformData payload.");
		}
		if (!CookedPlatformData.UnlockReadOnly(&OutError)) return FailCooked(OutError);
		PublishPayload(std::move(Candidate), false);

		OutError.clear();
		return true;
	}

	auto DTerrainHeightmap::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Terrain heightmap '{}' is not ready for a Win64 game cook.", GetObjectPath());
			return false;
		}
		if ((!Payload || !Payload->IsValid()) && !PostLoad(OutError)) return false;
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DTerrainHeightmap::PublishAssetImportData(
		DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "TerrainHeightmap import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

}
