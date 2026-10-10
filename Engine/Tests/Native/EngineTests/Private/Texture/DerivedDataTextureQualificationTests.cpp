#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "NativeQualificationSupport.h"
#include "DerivedDataBuildTestSupport.h"
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/VolumeTextureBuild.h"
#include "Texture/ITextureCompressorModule.h"
#include "Texture/Texture2DBuildOperations.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/TextureDerivedData.h"
#include "Runtime/Engine/Private/Texture/TextureBuildDiagnostics.h"
#include "Runtime/Engine/Private/Texture/TextureBuildSession.h"
#include "Runtime/Engine/Private/Texture/TextureCubeBuildFunction.h"
#include "Runtime/Engine/Private/Texture/VolumeTextureBuildFunction.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	// Executes the real recipes through the production module interface, observing
	// their returned allocations. Comparing them to the eventual product measures
	// payload copies after the recipe; it does not count recipe scratch or codecs.
	class FObservedTextureCompressorModule final : public ITextureCompressorModule
	{
	public:
		std::vector<FByteView> ReturnedBlocks;
		uint64 BuildCalls = 0;
		std::function<void()> OnRecipeCompleted;
		auto ResetObservation() -> void { ReturnedBlocks.clear(); BuildCalls = 0; }
		auto GetTexture2DBuilderVersion() const -> uint32 override { return Texture2DBuilderVersion; }
		auto GetTextureCubeBuilderVersion() const -> uint32 override { return TextureCubeBuilderVersion; }
		auto GetTextureCubeProjectionVersion() const -> uint32 override { return TextureCubeProjectionVersion; }
		auto GetVolumeTextureBuilderVersion() const -> uint32 override { return VolumeTextureBuilderVersion; }
		auto Observe(const FTexturePlatformData& Platform) -> void
		{
			for (const auto& Mip : Platform.Mips) ReturnedBlocks.emplace_back(Mip.Pixels);
		}
		auto BuildTexture2D(const FTexture2DBuildInput& Request)
			-> std::optional<FTexture2DBuildOutput> override
		{
			++BuildCalls;
			auto Built = Durin::BuildTexture2D(Request);
			if (Built)
			{
				Observe(Built->PlatformData);
				if (OnRecipeCompleted) OnRecipeCompleted();
			}
			return Built;
		}
		auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> override
		{
			return Durin::NormalizeTextureCube(Request);
		}
		auto BuildTextureCube(const FTextureCubeBuildInput& Request)
			-> std::optional<FTextureCubePlatformData> override
		{
			++BuildCalls;
			auto Built = Durin::BuildTextureCube(Request);
			if (Built)
			{
				for (const auto& Face : Built->Faces) Observe(Face);
				if (OnRecipeCompleted) OnRecipeCompleted();
			}
			return Built;
		}
		auto BuildVolumeTexture(const FVolumeTextureBuildInput& Request)
			-> std::optional<FVolumeTexturePlatformData> override
		{
			++BuildCalls;
			auto Built = Durin::BuildVolumeTexture(Request);
			if (Built)
			{
				for (const auto& Mip : Built->Mips) ReturnedBlocks.emplace_back(Mip.Voxels);
				if (OnRecipeCompleted) OnRecipeCompleted();
			}
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
		FObservedTextureCompressorModule* Provider = nullptr;
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			PreviousRoot = FPaths::DerivedDataCacheDir();
			Root = Testing::CreateTestFixtureDirectory("DerivedDataTextureBaseline");
			Log = std::make_unique<Testing::FQualificationLogSession>((Root / "Logs").generic_string());
			ASSERT_TRUE(Log->IsStarted());
			static auto* Installed = static_cast<FObservedTextureCompressorModule*>(FModuleTestHarness::InstallStartedModule(
				FName("TextureCompressor"), std::make_unique<FObservedTextureCompressorModule>()));
			Provider = Installed;
			ASSERT_NE(Provider, nullptr);
			ASSERT_EQ(ITextureCompressorModule::Get(), Provider);
			FModuleManager::Get().LoadModuleChecked("MeshBuilder");
		}
		auto TearDown() -> void override
		{
			if (Provider) Provider->OnRecipeCompleted = {};
			FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
			Log.reset();
			Testing::RemoveTestWorkDirectory(Root);
		}
		auto CheckCancellation(std::string_view Fixture, const DerivedData::FBuildDefinition& Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver) -> void
		{
			FPaths::SetDerivedDataCacheDirForTests((Root / Fixture).generic_string());
			Provider->ResetObservation();
			std::atomic<bool> bCancelled = false;
			Provider->OnRecipeCompleted = [&] { bCancelled.store(true); };
			DerivedData::FBuildRequestOptions Options;
			Options.Cancellation = DerivedData::FBuildCancellation([&] { return bCancelled.load(); });
			auto Canceled = TexturePrivate::Build(Definition, Resolver, std::move(Options));
			ASSERT_TRUE(Canceled);
			EXPECT_EQ(Canceled->GetStatus(), DerivedData::EStatus::Canceled);
			EXPECT_EQ(Canceled->GetOutput(), nullptr);
			EXPECT_EQ(Provider->BuildCalls, 1u);
			EXPECT_FALSE(Provider->ReturnedBlocks.empty());

			Provider->OnRecipeCompleted = {};
			auto Retried = TexturePrivate::Build(Definition, std::move(Resolver));
			ASSERT_TRUE(Retried);
			EXPECT_EQ(Retried->GetStatus(), DerivedData::EStatus::Ok);
			EXPECT_NE(Retried->GetOutput(), nullptr);
			EXPECT_EQ(Provider->BuildCalls, 2u);
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

TEST_F(FDerivedDataTextureQualificationTests, CancellationDiscardsCompletedRecipeProduct)
{
	auto Image = Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakePixels(4 * 4 * 4));
	ASSERT_TRUE(Image);
	const auto View = Image->GetView();
	auto Source = PrepareTexture2DSourceMipChain(std::span(&View, 1), 4, 1);
	ASSERT_TRUE(Source);
	const auto Request = MakeTexture2DBuildRequest(*Source);
	ASSERT_TRUE(Request);
	FPaths::SetDerivedDataCacheDirForTests((Root / "CanceledRecipe").generic_string());
	Provider->ResetObservation();
	std::atomic<bool> bCancelled = false;
	// Cancellation arrives after the synchronous recipe has produced valid data.
	Provider->OnRecipeCompleted = [&] { bCancelled.store(true); };
	const FTexture2DBuildExecutionControl Control{.ShouldCancel = [&] { return bCancelled.load(); }};
	FTexturePlatformData Product;
	FTexture2DBuildInputIdentity Identity;
	const auto Built = BuildTexture2DPlatformData(*Request, Product, Identity, &Control);
	ASSERT_FALSE(Built);
	EXPECT_EQ(Built.error().Code, ETexture2DBuildError::Cancelled);
	EXPECT_EQ(Provider->BuildCalls, 1u);
	EXPECT_FALSE(Provider->ReturnedBlocks.empty());
	EXPECT_FALSE(Product.IsValid());
	EXPECT_NE(Identity.BuilderVersion, 0u);

	// Canceled products must not be applied or persisted as a cache hit.
	Provider->OnRecipeCompleted = {};
	bCancelled.store(false);
	ASSERT_TRUE(BuildTexture2DPlatformData(*Request, Product, Identity, &Control));
	EXPECT_TRUE(Product.IsValid());
	EXPECT_EQ(Provider->BuildCalls, 2u);
}

TEST_F(FDerivedDataTextureQualificationTests, CubeCancellationDiscardsCompletedRecipeProduct)
{
	FTextureCubeFaceImages Faces;
	for (auto& Face : Faces.Faces)
	{
		auto Image = Image::FImage::TryCreate({.Width = 2, .Height = 2,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(16, std::byte{255}));
		ASSERT_TRUE(Image);
		Face = std::move(*Image);
	}
	Faces.SourceChannelCount = 4;
	auto Source = PrepareTextureCubeSource(Faces);
	ASSERT_TRUE(Source);
	auto Definition = TexturePrivate::MakeTextureCubeSessionDefinition({
		.CanonicalSourceIdentity = Source->GetIdentity(), .bSRGB = false,
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game});
	ASSERT_TRUE(Definition);
	CheckCancellation("CanceledCube", *Definition, TexturePrivate::MakeTextureCubeInputResolver(*Source, nullptr));
}

TEST_F(FDerivedDataTextureQualificationTests, VolumeCancellationDiscardsCompletedRecipeProduct)
{
	FVolumeTextureSourceData Voxels{.Width = 2, .Height = 2, .Depth = 2};
	ASSERT_TRUE(Voxels.SetVoxelBytes(FByteBuffer(8, std::byte{42})));
	auto Source = PrepareVolumeTextureSource(Voxels);
	ASSERT_TRUE(Source);
	const FVolumeTextureBuildRequest Request{.Source = std::move(*Source)};
	auto Definition = TexturePrivate::MakeVolumeTextureSessionDefinition(Request);
	ASSERT_TRUE(Definition);
	CheckCancellation("CanceledVolume", *Definition, TexturePrivate::MakeVolumeTextureInputResolver(Request.Source));
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
		FTexturePlatformData Product;
		FTexture2DBuildInputIdentity Identity;
		if (!BuildTexture2DPlatformData(Effective, Product, Identity)) return {};
		FXxHash128Builder Hash;
		const auto Bytes = HashMips(Product, Hash);
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		std::vector<FByteView> Blocks;
		for (const auto& Mip : Product.Mips) Blocks.emplace_back(Mip.Pixels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Request->Source.ReleaseSourceMemory(); });
}

TEST_F(FDerivedDataTextureQualificationTests, CubeColdAndWarmIncludingNormalization)
{
	FTextureCubeFacesBuildInput Input{.OriginalSourceWidth = 256, .OriginalSourceHeight = 256};
	for (auto& Face : Input.FaceImages.Faces)
	{
		auto Image = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, MakePixels(256 * 256 * 4));
		ASSERT_TRUE(Image);
		Face = std::move(*Image);
	}
	Input.FaceImages.SourceChannelCount = 4;
	const FTextureCubeBuildRequest Request{.Input = std::move(Input)};
	Measure("cube-6x256-rgba8", [&](bool bWrite) -> std::optional<FTextureBaselineValue> {
		auto Effective = Request;
		Effective.bPersistDerivedData = bWrite;
		auto Built = BuildTextureCubeDetached(Effective);
		if (!Built) return {};
		FXxHash128Builder Hash;
		uint64 Bytes = 0;
		for (const auto& Face : Built->PlatformData->Faces) Bytes += HashMips(Face, Hash);
		std::vector<FByteView> Blocks;
		for (const auto& Face : Built->PlatformData->Faces)
			for (const auto& Mip : Face.Mips) Blocks.emplace_back(Mip.Pixels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes,
			Provider->BuildCalls == 0, 0, false, Provider->CountTransferBytes(Blocks)};
	});
}

TEST_F(FDerivedDataTextureQualificationTests, CubeCapturedSourceColdAndWarm)
{
	FTextureCubeFaceImages Faces;
	for (auto& Face : Faces.Faces)
	{
		auto Image = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, MakePixels(256 * 256 * 4));
		ASSERT_TRUE(Image);
		Face = std::move(*Image);
	}
	Faces.SourceChannelCount = 4;
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
		for (const auto& Face : (*Built)->Faces)
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
		for (const auto& Face : (*Built)->Faces)
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
		for (const auto& Mip : (*Built)->Mips) { Hash.Update(Mip.Voxels); Bytes += Mip.Voxels.size(); }
		const bool bHit = Provider->BuildCalls == 0;
		const auto Requests = Probe->GetReadStats().RequestCount - Before;
		EXPECT_EQ(Requests, bHit ? 0u : 1u);
		std::vector<FByteView> Blocks;
		for (const auto& Mip : (*Built)->Mips) Blocks.emplace_back(Mip.Voxels);
		return FTextureBaselineValue{Hash.Finalize(), Bytes, bHit, Requests, true, Provider->CountTransferBytes(Blocks)};
	}, [&] { Request.Source.ReleaseSourceMemory(); });
}
