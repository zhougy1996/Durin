#include "EngineTestSupport.h"
#include <gtest/gtest.h>
#include "Texture/TextureBuilder.h"

// Explicit CPU qualification only; no latency assertions in correctness suites.
TEST(FTextureCompressionQualificationTests, SerialAndParallelCompression)
{
	InitializeDObjectSystem();
	using namespace Durin;
	FByteBuffer Pixels(1024 * 1024 * 4);
	uint32 Random = 12345;
	for (auto& Byte : Pixels)
	{
		Random = Random * 1664525u + 1013904223u;
		Byte = static_cast<std::byte>(Random >> 24);
	}
	auto ImageResult1 = Image::FImage::TryCreate({.Width = 1024, .Height = 1024,
		.Format = Image::ERawImageFormat::RGBA8}, Pixels);
	ASSERT_TRUE(ImageResult1);
	auto Source = std::move(*ImageResult1);
	for (auto Usage : {ETextureUsage::Color, ETextureUsage::Normal, ETextureUsage::DataMask})
	{
		std::array<std::vector<double>, 2> Samples;
		uint64 PeakIntermediateBytes = 0;
		uint64 RetainedMipBytes = 0;
		std::optional<FXxHash128> OutputHash;
		// One warm-up and three measured samples per mode, alternating order.
		for (int Round = 0; Round < 4; ++Round)
		for (int Order = 0; Order < 2; ++Order)
		{
			const int Mode = (Round + Order) % 2;
			const TextureBuilder::FBuildExecutionControl Control{
				.bParallelCompression = Mode != 0};
			auto Built = TextureBuilder::BuildMipChain({.SourceMips = std::span(&Source, 1),
				.Settings = {.Usage = Usage, .bSRGB = false},
				.PixelFormat = TextureBuilder::SelectPixelFormat(Usage, false, false)}, &Control);
			ASSERT_TRUE(Built);
			const auto& Platform = Built->PlatformData;
			const auto& Metrics = Built->Metrics;
			PeakIntermediateBytes = std::max(PeakIntermediateBytes, Metrics.PeakIntermediateBytes);
			uint64 Bytes = 0;
			FXxHash128Builder Hash;
			for (const auto& Mip : Platform.Mips)
			{
				Bytes += Mip.Pixels.size();
				Hash.Update(Mip.Pixels);
			}
			if (OutputHash) EXPECT_EQ(Hash.Finalize(), *OutputHash);
			else OutputHash = Hash.Finalize();
			RetainedMipBytes = Bytes;
			if (Round) Samples[Mode].push_back(Metrics.CompressionNanoseconds / 1e6);
		}
		for (auto& Values : Samples) std::ranges::sort(Values);
		std::cout << "Texture compression 1024x1024 usage=" << static_cast<int>(Usage)
			<< " serial_median_ms=" << Samples[0][1]
			<< " parallel_median_ms=" << Samples[1][1]
			<< " speedup=" << Samples[0][1] / Samples[1][1]
			<< " peak_intermediate_bytes=" << PeakIntermediateBytes
			<< " retained_mip_bytes=" << RetainedMipBytes
			<< " output_hash=" << OutputHash->ToString() << std::endl;
	}
}
