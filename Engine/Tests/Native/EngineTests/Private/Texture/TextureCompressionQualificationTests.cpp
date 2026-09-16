#include "EngineTestSupport.h"
#include <gtest/gtest.h>
#include "Texture/TextureBuilder.h"
#include <iostream>

// Explicit CPU qualification only; no latency assertions in correctness suites.
TEST(FTextureCompressionQualificationTests, SerialAndParallelCompression)
{
	InitializeDObjectSystem();
	using namespace Durin;
	Image::FImage Source;
	FByteBuffer Pixels(1024 * 1024 * 4);
	uint32 Random = 12345;
	for (auto& Byte : Pixels)
	{
		Random = Random * 1664525u + 1013904223u;
		Byte = static_cast<std::byte>(Random >> 24);
	}
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 1024, .Height = 1024,
		.Format = Image::ERawImageFormat::RGBA8}, Pixels, Source));
	for (auto Usage : {ETextureUsage::Color, ETextureUsage::Normal, ETextureUsage::DataMask})
	{
		std::array<std::vector<double>, 2> Samples;
		// One warm-up and three measured samples per mode, alternating order.
		for (int Round = 0; Round < 4; ++Round)
		for (int Order = 0; Order < 2; ++Order)
		{
			const int Mode = (Round + Order) % 2;
			TextureBuilder::FBuildMipChainMetrics Metrics;
			const TextureBuilder::FBuildExecutionControl Control{
				.Metrics = &Metrics, .bParallelCompression = Mode != 0};
			FTexturePlatformData Platform;
			ASSERT_TRUE(TextureBuilder::BuildMipChain(std::span(&Source, 1), Usage, false,
				Platform, 0, ETextureCompressionQuality::Normal,
				ETextureAlphaMipMode::Average, 0.5f, &Control, false));
			if (Round) Samples[Mode].push_back(Metrics.CompressionNanoseconds / 1e6);
		}
		for (auto& Values : Samples) std::ranges::sort(Values);
		std::cout << "Texture compression 1024x1024 usage=" << static_cast<int>(Usage)
			<< " serial_median_ms=" << Samples[0][1]
			<< " parallel_median_ms=" << Samples[1][1]
			<< " speedup=" << Samples[0][1] / Samples[1][1] << std::endl;
	}
}
