#include "Math/Color.h"

#include <gtest/gtest.h>

#include <limits>

TEST(FColorTests, ConvertsBetweenLinearAndSRGBTransferFunctions)
{
	EXPECT_DOUBLE_EQ(Durin::LinearToSRGB(0.0), 0.0);
	EXPECT_DOUBLE_EQ(Durin::LinearToSRGB(-1.0), 0.0);
	EXPECT_NEAR(Durin::LinearToSRGB(0.0031308), 0.040449936, 1.e-12);
	EXPECT_NEAR(Durin::LinearToSRGB(0.18), 0.46135612950044164, 1.e-12);
	EXPECT_DOUBLE_EQ(Durin::LinearToSRGB(1.0), 1.0);
	EXPECT_DOUBLE_EQ(Durin::LinearToSRGB(2.0), 1.0);
	EXPECT_DOUBLE_EQ(Durin::LinearToSRGB(std::numeric_limits<double>::quiet_NaN()), 0.0);

	EXPECT_DOUBLE_EQ(Durin::SRGBToLinear(0.0), 0.0);
	EXPECT_DOUBLE_EQ(Durin::SRGBToLinear(-1.0), 0.0);
	EXPECT_NEAR(Durin::SRGBToLinear(0.04045), 0.0031308049535603713, 1.e-15);
	EXPECT_NEAR(Durin::SRGBToLinear(0.5), 0.21404114048223255, 1.e-15);
	EXPECT_DOUBLE_EQ(Durin::SRGBToLinear(1.0), 1.0);
	EXPECT_DOUBLE_EQ(Durin::SRGBToLinear(2.0), 1.0);
	EXPECT_DOUBLE_EQ(Durin::SRGBToLinear(std::numeric_limits<double>::quiet_NaN()), 0.0);

	for (const double Linear : {0.0, 0.001, 0.01, 0.18, 0.5, 1.0})
	{
		EXPECT_NEAR(Durin::SRGBToLinear(Durin::LinearToSRGB(Linear)), Linear, 1.e-12);
	}
}

TEST(FColorTests, QuantizesNormalizedChannelsWithDefinedClamping)
{
	EXPECT_EQ(Durin::QuantizeUNorm8(-1.0), 0);
	EXPECT_EQ(Durin::QuantizeUNorm8(0.0), 0);
	EXPECT_EQ(Durin::QuantizeUNorm8(0.5), 128);
	EXPECT_EQ(Durin::QuantizeUNorm8(1.0), 255);
	EXPECT_EQ(Durin::QuantizeUNorm8(2.0), 255);
	EXPECT_EQ(Durin::QuantizeUNorm8(std::numeric_limits<double>::quiet_NaN()), 0);
}

TEST(FColorTests, EncodesLinearRGBAsSRGBAndAlphaAsLinearUNorm)
{
	const Durin::FColor Encoded = Durin::FLinearColor(0.0f, 0.18f, 0.5f, 0.5f).ToFColorSRGB();
	EXPECT_EQ(Encoded.R, 0);
	EXPECT_EQ(Encoded.G, 118);
	EXPECT_EQ(Encoded.B, 188);
	EXPECT_EQ(Encoded.A, 128);

	const Durin::FLinearColor Decoded(Encoded);
	EXPECT_NEAR(Decoded.G, 0.18f, 0.002f);
	EXPECT_NEAR(Decoded.B, 0.5f, 0.003f);
	EXPECT_NEAR(Decoded.A, 0.5f, 0.003f);
}
