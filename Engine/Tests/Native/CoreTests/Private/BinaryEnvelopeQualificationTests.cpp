#include "BinaryEnvelopeTestSupport.h"

#include <chrono>

TEST(FBinaryEnvelopeQualificationTests, HeaderValidationCostIsBoundedForSmallAndMaximumPolicySamples)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	const Durin::FByteBuffer Small = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
	const Durin::FByteBuffer Maximum = ReferenceEncode(
		FirstFormatId, 3, 1, Durin::FByteBuffer(4096 - 64), 4096);

	auto Measure = [&](Durin::FByteView Bytes, uint64 PhysicalBytes) {
		constexpr size_t Iterations = 2000;
		const auto Begin = std::chrono::steady_clock::now();
		for (size_t Index = 0; Index < Iterations; ++Index)
		{
			FValidatedBinaryEnvelope Output;
			require(ValidateBinaryEnvelopeHeader(
				Bytes, PhysicalBytes, TestLimits, Registry, Output));
		}
		return std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - Begin).count() / Iterations;
	};

	const double SmallMicroseconds = Measure(Small, 64);
	const double MaximumMicroseconds = Measure(Maximum, 4096);
	testing::Test::RecordProperty("small_header_bytes", Small.size());
	testing::Test::RecordProperty("small_parse_us", SmallMicroseconds);
	testing::Test::RecordProperty("maximum_header_bytes", Maximum.size());
	testing::Test::RecordProperty("maximum_parse_us", MaximumMicroseconds);
	EXPECT_LT(SmallMicroseconds, 100.0);
	EXPECT_LT(MaximumMicroseconds, 250.0);
}
