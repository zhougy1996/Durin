#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "NativeQualificationSupport.h"
#include "DerivedDataBuildTestSupport.h"
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/VolumeTextureBuild.h"
#include "Texture/ITextureBuildModule.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/TextureDerivedData.h"
#include "Runtime/Engine/Private/Texture/TextureBuildDiagnostics.h"
#include <gtest/gtest.h>
#include <iostream>

namespace
{
	using namespace Durin;

	// Executes the real recipes through the production module interface, observing
	// their returned allocations. Comparing them to the eventual product measures
	// payload copies after the recipe; it does not count recipe scratch or codecs.
	class FObservedTextureBuildModule final : public ITextureBuildModule
	{
	public:
		std::vector<FByteView> ReturnedBlocks;
		uint64 BuildCalls = 0;
		auto ResetObservation() -> void { ReturnedBlocks.clear(); BuildCalls = 0; }
		auto GetTexture2DBuilderVersion() const -> uint32 override { return Texture2DBuilderVersion; }
		auto GetTextureCubeBuilderVersion() const -> uint32 override { return TextureCubeBuilderVersion; }
		auto GetTextureCubeProjectionVersion() const -> uint32 override { return TextureCubeProjectionVersion; }
		auto GetVolumeTextureBuilderVersion() const -> uint32 override { return VolumeTextureBuilderVersion; }
		auto Observe(const FTexturePlatformData& Platform) -> void
		{
			for (const auto& Mip : Platform.Mips) ReturnedBlocks.emplace_back(Mip.Pixels);
		}
		auto BuildTexture2D(const FTexture2DBuildInput& Request, const FTexture2DBuildControl* Control)
			-> std::expected<FTexture2DBuildOutput, FTexture2DBuildError> override
		{
			++BuildCalls;
			auto Built = Durin::BuildTexture2D(Request, Control);
			if (Built) Observe(Built->PlatformData);
			return Built;
		}
		auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return Durin::NormalizeTextureCube(Request);
		}
		auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError> override
		{
			++BuildCalls;
			auto Built = Durin::BuildTextureCube(Request);
			if (Built) for (const auto& Face : (*Built)->Faces) Observe(Face);
			return Built;
		}
		auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, FTextureBuildError> override
		{
			++BuildCalls;
			auto Built = Durin::BuildVolumeTexture(Request);
			if (Built) for (const auto& Mip : (*Built)->Mips) ReturnedBlocks.emplace_back(Mip.Voxels);
			return Built;
		}
		auto CountTransferBytes(std::span<const FByteView> FinalBlocks) const -> uint64
		{
			if (!BuildCalls) return 0;
			EXPECT_EQ(FinalBlocks.size(), ReturnedBlocks.size());
			uint64 CopiedBytes = 0;
			for (size_t Index = 0; Index < FinalBlocks.size(); ++Index)
				if (Index >= ReturnedBlocks.size() || ReturnedBlocks[Index].data() != FinalBlocks[Index].data()
					|| ReturnedBlocks[Index].size() != FinalBlocks[Index].size()) CopiedBytes += FinalBlocks[Index].size();
			return CopiedBytes;
		}
	};

	struct FTextureBaselineValue
	{
		FXxHash128 Hash;
		uint64 Bytes = 0;
		bool bHit = false;
		uint64 SourceRequests = 0;
		bool bSourceProbe = false;
		uint64 TransferCopiedBytes = 0;
	};

	// Source preparation is outside the measured interval. Each round has a
	// fresh cache root; the immediately following call must be a real DDC hit.
	class FDerivedDataTextureQualificationTests : public testing::Test
	{
	protected:
		std::string PreviousRoot;
		std::filesystem::path Root;
		std::unique_ptr<Testing::FQualificationLogSession> Log;
		FObservedTextureBuildModule* Provider = nullptr;
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			PreviousRoot = FPaths::DerivedDataCacheDir();
			Root = Testing::CreateTestFixtureDirectory("DerivedDataTextureBaseline");
			Log = std::make_unique<Testing::FQualificationLogSession>((Root / "Logs").generic_string());
			ASSERT_TRUE(Log->IsStarted());
			static auto* Installed = static_cast<FObservedTextureBuildModule*>(FModuleTestHarness::InstallStartedModule(
				FName("TextureBuild"), std::make_unique<FObservedTextureBuildModule>()));
			Provider = Installed;
			ASSERT_NE(Provider, nullptr);
			ASSERT_EQ(ITextureBuildModule::Get(), Provider);
			FModuleManager::Get().LoadModuleChecked("MeshBuilder");
		}
		auto TearDown() -> void override
		{
			FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
			Log.reset();
			Testing::RemoveTestWorkDirectory(Root);
		}
		template<typename TBuild>
		auto Measure(std::string_view Fixture, TBuild&& Build, const std::function<void()>& Prepare = {}) -> void
		{
			std::array<std::vector<double>, 3> Samples;
			std::array<uint64, 3> PeakIncrease{}, PeakAllocated{}, SourceRequests{};
			bool bAllocationSampleAvailable = false;
			std::optional<FTextureBaselineValue> Reference;
			for (uint32 Round = 0; Round < 4; ++Round)
			{
				FPaths::SetDerivedDataCacheDirForTests((Root / std::to_string(Round)).generic_string());
				for (uint32 Warm = 0; Warm < 3; ++Warm)
				{
					if (Warm == 2) FPaths::SetDerivedDataCacheDirForTests((Root / std::to_string(Round) / "NoWrite").generic_string());
					if (Prepare) Prepare();
					Provider->ResetObservation();
					Testing::FQualificationAllocationSampler Allocations;
					const auto Start = std::chrono::steady_clock::now();
					auto Value = Build(Warm != 2);
					const auto End = std::chrono::steady_clock::now();
					const auto Allocation = Allocations.Finish();
					ASSERT_TRUE(Value);
					ASSERT_EQ(Value->bHit, Warm == 1);
					EXPECT_EQ(Provider->BuildCalls, Warm == 1 ? 0u : 1u);
					EXPECT_EQ(Value->TransferCopiedBytes, 0u);
					ASSERT_GT(Value->Bytes, 0u);
					if (Reference)
					{
						EXPECT_EQ(Value->Hash, Reference->Hash);
						EXPECT_EQ(Value->Bytes, Reference->Bytes);
					}
					else Reference = *Value;
					if (Round)
					{
						Samples[Warm].push_back(std::chrono::duration<double, std::milli>(End - Start).count());
						PeakIncrease[Warm] = std::max(PeakIncrease[Warm], Allocation.GetPeakIncrease());
						PeakAllocated[Warm] = std::max(PeakAllocated[Warm], Allocation.PeakBytes);
						SourceRequests[Warm] = std::max(SourceRequests[Warm], Value->SourceRequests);
						bAllocationSampleAvailable = Allocation.bAvailable;
					}
				}
			}
			for (auto& Values : Samples) std::ranges::sort(Values);
			std::cout << "derived_data_texture fixture=" << Fixture
				<< " cold_median_ms=" << Samples[0][1] << " warm_median_ms=" << Samples[1][1]
				<< " no_write_median_ms=" << Samples[2][1]
				<< " output_bytes=" << Reference->Bytes << " output_hash=" << Reference->Hash.ToString()
				<< " cold_source_requests=" << SourceRequests[0] << " warm_source_requests=" << SourceRequests[1]
				<< " source_request_probe=" << Reference->bSourceProbe
				<< " allocation_sample_available=" << bAllocationSampleAvailable
				<< " cold_sampled_zone_peak_bytes=" << PeakAllocated[0] << " warm_sampled_zone_peak_bytes=" << PeakAllocated[1]
				<< " cold_sampled_zone_increase_bytes=" << PeakIncrease[0] << " warm_sampled_zone_increase_bytes=" << PeakIncrease[1]
				<< " no_write_sampled_zone_increase_bytes=" << PeakIncrease[2]
				<< " recipe_to_product_copied_bytes=0 warmup_rounds=1 measured_rounds=3 includes_output_hash=true" << std::endl;
		}
	};

	auto MakePixels(uint32 Bytes) -> FByteBuffer
	{
		FByteBuffer Result(Bytes);
		uint32 Random = 12345;
		for (auto& Byte : Result)
		{
			Random = Random * 1664525u + 1013904223u;
			Byte = static_cast<std::byte>(Random >> 24);
		}
		return Result;
	}

	auto HashMips(const FTexturePlatformData& Data, FXxHash128Builder& Hash) -> uint64
	{
		uint64 Bytes = 0;
		for (const auto& Mip : Data.Mips) { Hash.Update(Mip.Pixels); Bytes += Mip.Pixels.size(); }
		return Bytes;
	}
}

TEST_F(FDerivedDataTextureQualificationTests, Texture2DColdAndWarm)
{
	auto Image = Image::FImage::TryCreate({.Width = 1024, .Height = 1024,
		.Format = Image::ERawImageFormat::RGBA8}, MakePixels(1024 * 1024 * 4));
	ASSERT_TRUE(Image);
	const auto View = Image->GetView();
	auto Source = PrepareTexture2DSourceMipChain(std::span(&View, 1), 4, 1);
	ASSERT_TRUE(Source);
	Source->ReleaseSourceMemory();
	const auto Probe = Testing::AttachBuildSourceReadProbe(*Source, Source->GetBulkData(), "Payload");
	ASSERT_TRUE(Probe);
	const auto Request = MakeTexture2DBuildRequest(*Source, {.Usage = ETextureUsage::DataMask});
	ASSERT_TRUE(Request);
	Measure("2d-1024-rgba8-bc7", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		auto Effective = *Request;
		Effective.bPersistDerivedData = bWrite;
		const auto Before = Probe->GetReadStats().RequestCount;
		FTexture2DBuildProduct Product;
		FTexture2DBuildInputIdentity Identity;
		if (!BuildTexture2DPlatformData(Effective, Product, Identity)) return {};
		FXxHash128Builder Hash;
		const auto Bytes = HashMips(Product.PlatformData, Hash);
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		std::vector<FByteView> Blocks;
		for (const auto& Mip : Product.PlatformData.Mips) Blocks.emplace_back(Mip.Pixels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Request->Source.ReleaseSourceMemory(); });
}

TEST_F(FDerivedDataTextureQualificationTests, CubeColdAndWarmIncludingNormalization)
{
	FTextureCubeFacesBuildInput Input{.OriginalSourceWidth = 256, .OriginalSourceHeight = 256};
	for (auto& Face : Input.DecodedFaces.Faces)
	{
		auto Image = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, MakePixels(256 * 256 * 4));
		ASSERT_TRUE(Image);
		Face = std::move(*Image);
	}
	Input.DecodedFaces.SourceChannelCounts.fill(4);
	const FTextureCubeBuildRequest Request{.Input = std::move(Input)};
	Measure("cube-6x256-rgba8", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		auto Effective = Request;
		Effective.bPersistDerivedData = bWrite;
		auto Built = BuildTextureCubeDetached(Effective);
		if (!Built) return {};
		FXxHash128Builder Hash;
		uint64 Bytes = 0;
		for (const auto& Face : Built->Product.PlatformData->Faces) Bytes += HashMips(Face, Hash);
		std::vector<FByteView> Blocks;
		for (const auto& Face : Built->Product.PlatformData->Faces)
			for (const auto& Mip : Face.Mips) Blocks.emplace_back(Mip.Pixels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes,
			Provider->BuildCalls == 0, 0, false, Provider->CountTransferBytes(Blocks)};
	});
}

TEST_F(FDerivedDataTextureQualificationTests, CubeCapturedSourceColdAndWarm)
{
	FTextureCubeDecodedFaces Faces;
	for (auto& Face : Faces.Faces)
	{
		auto Image = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, MakePixels(256 * 256 * 4));
		ASSERT_TRUE(Image);
		Face = std::move(*Image);
	}
	Faces.SourceChannelCounts.fill(4);
	auto Source = PrepareTextureCubeSource(Faces);
	ASSERT_TRUE(Source);
	Source->ReleaseSourceMemory();
	const auto Probe = Testing::AttachBuildSourceReadProbe(*Source, Source->GetBulkData(), "Payload");
	ASSERT_TRUE(Probe);
	Measure("cube-captured-6x256-rgba8", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		const auto Before = Probe->GetReadStats().RequestCount;
		auto Built = TexturePrivate::BuildTextureCubeSource(*Source, false, 0, 0.0f,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, bWrite);
		if (!Built) { ADD_FAILURE() << Built.error().Diagnostic; return {}; }
		FXxHash128Builder Hash;
		uint64 Bytes = 0;
		std::vector<FByteView> Blocks;
		for (const auto& Face : Built->PlatformData->Faces)
		{
			Bytes += HashMips(Face, Hash);
			for (const auto& Mip : Face.Mips) Blocks.emplace_back(Mip.Pixels);
		}
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Source->ReleaseSourceMemory(); });
}

TEST_F(FDerivedDataTextureQualificationTests, CubeCapturedPanoramaColdAndWarm)
{
	constexpr uint32 Width = 512, Height = 256;
	FByteBuffer Pixels(Width * Height * 4 * sizeof(float));
	for (uint32 Index = 0; Index < Width * Height; ++Index)
	{
		const std::array<float, 4> Pixel{static_cast<float>(Index % Width) / Width,
			static_cast<float>(Index / Width) / Height, 2.0f, 1.0f};
		std::memcpy(Pixels.data() + Index * sizeof(Pixel), Pixel.data(), sizeof(Pixel));
	}
	auto Panorama = Image::FImage::TryCreate({.Width = Width, .Height = Height,
		.Format = Image::ERawImageFormat::RGBA32F, .GammaSpace = Image::EImageGammaSpace::Linear}, std::move(Pixels));
	ASSERT_TRUE(Panorama);
	auto Source = PrepareTextureCubePanoramaSource(Panorama->GetView(), 4, 0);
	ASSERT_TRUE(Source);
	Source->ReleaseSourceMemory();
	const auto Probe = Testing::AttachBuildSourceReadProbe(*Source, Source->GetBulkData(), "Payload");
	ASSERT_TRUE(Probe);
	Measure("cube-captured-panorama-512x256-rgba32f", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		const auto Before = Probe->GetReadStats().RequestCount;
		auto Built = TexturePrivate::BuildTextureCubeSource(*Source, false, 128, 0.0f,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, bWrite);
		if (!Built) { ADD_FAILURE() << Built.error().Diagnostic; return {}; }
		FXxHash128Builder Hash;
		uint64 Bytes = 0;
		std::vector<FByteView> Blocks;
		for (const auto& Face : Built->PlatformData->Faces)
		{
			Bytes += HashMips(Face, Hash);
			for (const auto& Mip : Face.Mips) Blocks.emplace_back(Mip.Pixels);
		}
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Source->ReleaseSourceMemory(); });
}

TEST_F(FDerivedDataTextureQualificationTests, VolumeColdAndWarm)
{
	FVolumeTextureSourceData Input{.Width = 64, .Height = 64, .Depth = 64};
	ASSERT_TRUE(Input.SetVoxelBytes(MakePixels(64 * 64 * 64)));
	auto Source = PrepareVolumeTextureSource(Input);
	ASSERT_TRUE(Source);
	Source->ReleaseSourceMemory();
	const auto Probe = Testing::AttachBuildSourceReadProbe(*Source, Source->GetBulkData(), "Payload");
	ASSERT_TRUE(Probe);
	const FVolumeTextureBuildRequest Request{.Source = std::move(*Source)};
	Measure("volume-64x64x64-r8", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		auto Effective = Request;
		Effective.bPersistDerivedData = bWrite;
		const auto Before = Probe->GetReadStats().RequestCount;
		auto Built = BuildVolumeTextureDetached(Effective);
		if (!Built) return {};
		FXxHash128Builder Hash;
		uint64 Bytes = 0;
		for (const auto& Mip : Built->PlatformData->Mips) { Hash.Update(Mip.Voxels); Bytes += Mip.Voxels.size(); }
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		std::vector<FByteView> Blocks;
		for (const auto& Mip : Built->PlatformData->Mips) Blocks.emplace_back(Mip.Voxels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Request.Source.ReleaseSourceMemory(); });
}
