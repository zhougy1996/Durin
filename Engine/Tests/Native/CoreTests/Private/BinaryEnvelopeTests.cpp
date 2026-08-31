#include "Serialization/BinaryEnvelope.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	constexpr FGuid FirstFormatId{0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff};
	constexpr FGuid SecondFormatId{0xfedcba98, 0x76544321, 0x81234567, 0x89abcdef};
	constexpr FBinaryEnvelopeLimits TestLimits{4096, 16384};

	template<typename T>
	auto ReferenceWrite(std::span<std::byte> Bytes, size_t Offset, T Value) -> void
	{
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
	}

	template<typename T>
	auto ReferenceRead(std::span<const std::byte> Bytes, size_t Offset) -> T
	{
		T Value = 0;
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
		return Value;
	}

	auto ReferenceEncode(FGuid FormatId, uint32 FormatVersion, uint32 RequiredFeatures,
		std::span<const std::byte> FormatHeader, uint64 FileBytes) -> Durin::FByteArray
	{
		Durin::FByteArray Bytes(64 + FormatHeader.size());
		Bytes[0] = std::byte{0x44};
		Bytes[1] = std::byte{0x55};
		Bytes[2] = std::byte{0x52};
		Bytes[3] = std::byte{0x46};
		ReferenceWrite<uint16>(Bytes, 4, 1);
		ReferenceWrite<uint16>(Bytes, 6, 64);
		ReferenceWrite<uint32>(Bytes, 8, FormatId.A);
		ReferenceWrite<uint32>(Bytes, 12, FormatId.B);
		ReferenceWrite<uint32>(Bytes, 16, FormatId.C);
		ReferenceWrite<uint32>(Bytes, 20, FormatId.D);
		ReferenceWrite<uint32>(Bytes, 24, FormatVersion);
		ReferenceWrite<uint32>(Bytes, 28, RequiredFeatures);
		ReferenceWrite<uint64>(Bytes, 32, Bytes.size());
		ReferenceWrite<uint64>(Bytes, 40, FileBytes);
		std::ranges::copy(FormatHeader, Bytes.begin() + 64);
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		ReferenceWrite<uint64>(Bytes, 48, Hash.HashLow);
		ReferenceWrite<uint64>(Bytes, 56, Hash.HashHigh);
		return Bytes;
	}

	auto ReferenceParse(std::span<const std::byte> Bytes, uint64 PhysicalFileBytes,
		FBinaryEnvelopePreamble& OutPreamble) -> bool
	{
		if (Bytes.size() < 64 || Bytes[0] != std::byte{0x44} || Bytes[1] != std::byte{0x55}
			|| Bytes[2] != std::byte{0x52} || Bytes[3] != std::byte{0x46}
			|| ReferenceRead<uint16>(Bytes, 4) != 1 || ReferenceRead<uint16>(Bytes, 6) != 64)
			return false;
		FBinaryEnvelopePreamble Parsed{
			.FormatId = {
				ReferenceRead<uint32>(Bytes, 8), ReferenceRead<uint32>(Bytes, 12),
				ReferenceRead<uint32>(Bytes, 16), ReferenceRead<uint32>(Bytes, 20)},
			.FormatVersion = ReferenceRead<uint32>(Bytes, 24),
			.RequiredFeatures = ReferenceRead<uint32>(Bytes, 28),
			.HeaderBytes = ReferenceRead<uint64>(Bytes, 32),
			.FileBytes = ReferenceRead<uint64>(Bytes, 40),
			.HeaderHash = {
				ReferenceRead<uint64>(Bytes, 48), ReferenceRead<uint64>(Bytes, 56)}};
		if (!Parsed.FormatId.IsValid() || Parsed.FormatVersion == 0
			|| Parsed.HeaderBytes != Bytes.size() || Parsed.HeaderBytes < 64
			|| Parsed.HeaderBytes > Parsed.FileBytes || Parsed.FileBytes != PhysicalFileBytes)
			return false;
		Durin::FByteArray Zeroed(Bytes.begin(), Bytes.end());
		std::ranges::fill(std::span(Zeroed).subspan(48, 16), std::byte{});
		if (FXxHash128::HashBuffer(Zeroed) != Parsed.HeaderHash) return false;
		OutPreamble = Parsed;
		return true;
	}

	auto Hex(std::span<const std::byte> Bytes) -> std::string
	{
		std::string Result;
		for (std::byte Byte : Bytes) Result += std::format("{:02x}", std::to_integer<uint8>(Byte));
		return Result;
	}

	auto ReferenceRehash(Durin::FByteArray& Bytes) -> void
	{
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
		ReferenceWrite<uint64>(Bytes, 48, Hash.HashLow);
		ReferenceWrite<uint64>(Bytes, 56, Hash.HashHigh);
	}

	auto MakeDescriptor(FGuid FormatId = FirstFormatId,
		std::string Name = "Durin.BinaryFormat.Test") -> FBinaryFormatDescriptor
	{
		return {
			.FormatId = FormatId,
			.DebugName = std::move(Name),
			.MinimumFormatVersion = 2,
			.MaximumFormatVersion = 4,
			.SupportedRequiredFeatures = 0x00000005,
			.Limits = TestLimits};
	}

	auto MakeRegistry(std::span<const FBinaryFormatDescriptor> Descriptors)
		-> FBinaryFormatRegistry
	{
		FBinaryFormatRegistry Registry;
		EXPECT_TRUE(FBinaryFormatRegistry::Create(Descriptors, Registry));
		return Registry;
	}
}

TEST(FBinaryEnvelopeTests, FieldTableAndIndependentGoldensFreezeTheWireContract)
{
	EXPECT_EQ(BinaryEnvelopeHeaderVersion, 1);
	EXPECT_EQ(BinaryEnvelopePreambleBytes, 64);

	const Durin::FByteArray Minimal = ReferenceEncode(FirstFormatId, 2, 0, {}, 64);
	const std::array FormatHeader{
		std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
		std::byte{0x31}, std::byte{0x41}, std::byte{0x59}};
	const Durin::FByteArray Extended = ReferenceEncode(
		FirstFormatId, 0x01020304, 0xa1b2c3d4, FormatHeader, 0x0000000100000047ull);

	EXPECT_EQ(Hex(Minimal),
		"44555246010040003322110077665544bbaa9988ffeeddcc0200000000000000"
		"40000000000000004000000000000000540fcaaebe2014a06192544ae7796d78");
	EXPECT_EQ(Hex(Extended),
		"44555246010040003322110077665544bbaa9988ffeeddcc04030201d4c3b2a1"
		"470000000000000047000000010000008498987c1f692d3ad0d7038e70f2c431"
		"deadbeef314159");

	FBinaryEnvelopePreamble Parsed;
	ASSERT_TRUE(ReferenceParse(Minimal, Minimal.size(), Parsed));
	EXPECT_EQ(Parsed.FormatId, FirstFormatId);
	EXPECT_EQ(Parsed.HeaderBytes, 64);
}

TEST(FBinaryEnvelopeTests, ProductionEncodingFinalizationAndReferenceParserAgree)
{
	const std::array FormatHeader{
		std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
	const Durin::FByteArray Reference = ReferenceEncode(FirstFormatId, 3, 1, FormatHeader, 96);
	Durin::FByteArray Production(Reference.size(), std::byte{0x7b});
	const FBinaryEnvelopePreamble Preamble{
		.FormatId = FirstFormatId,
		.FormatVersion = 3,
		.RequiredFeatures = 1,
		.HeaderBytes = Production.size(),
		.FileBytes = 96};
	ASSERT_TRUE(EncodeBinaryEnvelopePreamble(Preamble, Production));
	std::ranges::copy(FormatHeader, Production.begin() + 64);
	ASSERT_TRUE(FinalizeBinaryEnvelopeHeader(Production, 96, TestLimits));
	EXPECT_EQ(Production, Reference);

	FBinaryEnvelopePreamble Parsed;
	EXPECT_TRUE(ReferenceParse(Production, 96, Parsed));
	EXPECT_EQ(Parsed.RequiredFeatures, 1);

	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	FValidatedBinaryEnvelope Validated;
	ASSERT_TRUE(ValidateBinaryEnvelopeHeader(
		Production, 96, TestLimits, Registry, Validated));
	EXPECT_EQ(Validated.Preamble.FormatId, FirstFormatId);
	EXPECT_TRUE(std::ranges::equal(Validated.FormatHeaderBytes, FormatHeader));
}

TEST(FBinaryEnvelopeTests, PrefixAndCompleteValidationAreSuccessAtomicAndBounded)
{
	const Durin::FByteArray Bytes = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
	const FBinaryEnvelopePreamble PreambleSentinel{
		.FormatId = SecondFormatId, .FormatVersion = 99, .HeaderBytes = 64, .FileBytes = 64};
	FBinaryEnvelopePreamble Preamble = PreambleSentinel;
	FBinaryEnvelopeDiagnostic Diagnostic;
	EXPECT_FALSE(ParseBinaryEnvelopePrefix(
		std::span(Bytes).first(63), 64, TestLimits, Preamble, &Diagnostic));
	EXPECT_EQ(Diagnostic.Error, EBinaryEnvelopeError::Truncated);
	EXPECT_EQ(Preamble.FormatId, PreambleSentinel.FormatId);

	ASSERT_TRUE(ParseBinaryEnvelopePrefix(Bytes, 64, TestLimits, Preamble, &Diagnostic));
	EXPECT_EQ(Preamble.HeaderBytes, 64);
	EXPECT_EQ(Diagnostic.Error, EBinaryEnvelopeError::None);

	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	FValidatedBinaryEnvelope Output{.Preamble = PreambleSentinel};
	Durin::FByteArray Corrupt = Bytes;
	Corrupt[0] ^= std::byte{1};
	EXPECT_FALSE(ValidateBinaryEnvelopeHeader(
		Corrupt, 64, TestLimits, Registry, Output, &Diagnostic));
	EXPECT_EQ(Diagnostic.Error, EBinaryEnvelopeError::InvalidMagic);
	EXPECT_EQ(Output.Preamble.FormatId, PreambleSentinel.FormatId);

	std::array<std::byte, 63> Destination;
	std::ranges::fill(Destination, std::byte{0x5a});
	EXPECT_FALSE(EncodeBinaryEnvelopePreamble(Preamble, Destination, &Diagnostic));
	EXPECT_TRUE(std::ranges::all_of(Destination,
		[](std::byte Byte) { return Byte == std::byte{0x5a}; }));
}

TEST(FBinaryEnvelopeTests, RegistryRejectsInvalidAndDuplicateDescriptorsInEitherOrder)
{
	FBinaryFormatRegistry Sentinel;
	const std::array Initial{MakeDescriptor()};
	ASSERT_TRUE(FBinaryFormatRegistry::Create(Initial, Sentinel));
	FBinaryEnvelopeDiagnostic Diagnostic;

	auto ExpectRejected = [&](std::span<const FBinaryFormatDescriptor> Descriptors,
		EBinaryEnvelopeError Error) {
		EXPECT_FALSE(FBinaryFormatRegistry::Create(Descriptors, Sentinel, &Diagnostic));
		EXPECT_EQ(Diagnostic.Error, Error);
		EXPECT_NE(Sentinel.Find(FirstFormatId), nullptr);
	};

	FBinaryFormatDescriptor Invalid = MakeDescriptor();
	Invalid.Limits.MaximumHeaderBytes = 0;
	ExpectRejected(std::span(&Invalid, 1), EBinaryEnvelopeError::InvalidDescriptor);
	Invalid = MakeDescriptor();
	Invalid.FormatId = {};
	ExpectRejected(std::span(&Invalid, 1), EBinaryEnvelopeError::InvalidDescriptor);
	Invalid = MakeDescriptor();
	Invalid.DebugName.clear();
	ExpectRejected(std::span(&Invalid, 1), EBinaryEnvelopeError::InvalidDescriptor);
	Invalid = MakeDescriptor();
	Invalid.MinimumFormatVersion = 5;
	Invalid.MaximumFormatVersion = 4;
	ExpectRejected(std::span(&Invalid, 1), EBinaryEnvelopeError::InvalidDescriptor);

	const std::array DuplicateIds{
		MakeDescriptor(FirstFormatId, "Format.First"),
		MakeDescriptor(FirstFormatId, "Format.Second")};
	ExpectRejected(DuplicateIds, EBinaryEnvelopeError::DuplicateFormatIdentity);
	const std::array ReversedDuplicateIds{DuplicateIds[1], DuplicateIds[0]};
	ExpectRejected(ReversedDuplicateIds, EBinaryEnvelopeError::DuplicateFormatIdentity);

	const std::array DuplicateNames{
		MakeDescriptor(FirstFormatId, "Format.Same"),
		MakeDescriptor(SecondFormatId, "Format.Same")};
	ExpectRejected(DuplicateNames, EBinaryEnvelopeError::DuplicateFormatName);

	const std::array Ordered{
		MakeDescriptor(FirstFormatId, "Format.First"),
		MakeDescriptor(SecondFormatId, "Format.Second")};
	const std::array Reversed{Ordered[1], Ordered[0]};
	FBinaryFormatRegistry ForwardRegistry;
	FBinaryFormatRegistry ReverseRegistry;
	ASSERT_TRUE(FBinaryFormatRegistry::Create(Ordered, ForwardRegistry));
	ASSERT_TRUE(FBinaryFormatRegistry::Create(Reversed, ReverseRegistry));
	EXPECT_EQ(ForwardRegistry.Find(FirstFormatId)->DebugName,
		ReverseRegistry.Find(FirstFormatId)->DebugName);

	std::atomic<bool> bConsistent = true;
	std::vector<std::thread> Readers;
	for (size_t ThreadIndex = 0; ThreadIndex < 8; ++ThreadIndex)
		Readers.emplace_back([&] {
			for (size_t Index = 0; Index < 1000; ++Index)
				if (!ForwardRegistry.Find(FirstFormatId)
					|| ForwardRegistry.Find(SecondFormatId)->DebugName != "Format.Second")
					bConsistent.store(false, std::memory_order_relaxed);
		});
	for (std::thread& Reader : Readers) Reader.join();
	EXPECT_TRUE(bConsistent.load(std::memory_order_relaxed));
}

TEST(FBinaryEnvelopeTests, PrefixDiagnosticsCoverVersionsLimitsIdentityAndExtremeExtents)
{
	FBinaryEnvelopeDiagnostic Diagnostic;
	FBinaryEnvelopePreamble Output{
		.FormatId = SecondFormatId, .FormatVersion = 99, .HeaderBytes = 64, .FileBytes = 64};
	const FBinaryEnvelopePreamble Sentinel = Output;
	auto ExpectError = [&](const Durin::FByteArray& Bytes, uint64 PhysicalBytes,
		const FBinaryEnvelopeLimits& Limits, EBinaryEnvelopeError Error) {
		Output = Sentinel;
		EXPECT_FALSE(ParseBinaryEnvelopePrefix(Bytes, PhysicalBytes, Limits, Output, &Diagnostic));
		EXPECT_EQ(Diagnostic.Error, Error);
		EXPECT_EQ(Output.FormatId, Sentinel.FormatId);
	};

	Durin::FByteArray Bytes = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	Bytes[4] = std::byte{2};
	ExpectError(Bytes, 64, TestLimits, EBinaryEnvelopeError::UnsupportedHeaderVersion);
	Bytes = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	Bytes[6] = std::byte{63};
	ExpectError(Bytes, 64, TestLimits, EBinaryEnvelopeError::InvalidPreambleSize);
	ExpectError(ReferenceEncode({}, 3, 0, {}, 64), 64, TestLimits,
		EBinaryEnvelopeError::InvalidFormatIdentity);
	ExpectError(ReferenceEncode(FirstFormatId, 3, 0, {}, 64), 64, {63, 64},
		EBinaryEnvelopeError::InvalidLimits);

	Bytes = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	ReferenceWrite<uint64>(Bytes, 32, 63);
	ReferenceRehash(Bytes);
	ExpectError(Bytes, 64, TestLimits, EBinaryEnvelopeError::InvalidExtent);
	Bytes = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	ReferenceWrite<uint64>(Bytes, 32, std::numeric_limits<uint64>::max());
	ReferenceWrite<uint64>(Bytes, 40, std::numeric_limits<uint64>::max());
	ReferenceRehash(Bytes);
	ExpectError(Bytes, std::numeric_limits<uint64>::max(), TestLimits,
		EBinaryEnvelopeError::InvalidExtent);
	ExpectError(ReferenceEncode(FirstFormatId, 3, 0, {}, 65), 64, TestLimits,
		EBinaryEnvelopeError::FileSizeMismatch);
}

TEST(FBinaryEnvelopeTests, VersionFeaturesExtentsIdentityAndHashFailClosed)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	FBinaryEnvelopeDiagnostic Diagnostic;
	FValidatedBinaryEnvelope Output;

	auto ExpectError = [&](const Durin::FByteArray& Bytes, uint64 PhysicalBytes,
		EBinaryEnvelopeError Error) {
		EXPECT_FALSE(ValidateBinaryEnvelopeHeader(
			Bytes, PhysicalBytes, TestLimits, Registry, Output, &Diagnostic));
		EXPECT_EQ(Diagnostic.Error, Error);
	};

	ExpectError(ReferenceEncode(SecondFormatId, 3, 0, {}, 64), 64,
		EBinaryEnvelopeError::UnknownFormat);
	ExpectError(ReferenceEncode(FirstFormatId, 5, 0, {}, 64), 64,
		EBinaryEnvelopeError::UnsupportedFormatVersion);
	ExpectError(ReferenceEncode(FirstFormatId, 3, 0x8, {}, 64), 64,
		EBinaryEnvelopeError::UnsupportedRequiredFeatures);
	ExpectError(ReferenceEncode(FirstFormatId, 3, 0, {}, 65), 64,
		EBinaryEnvelopeError::FileSizeMismatch);

	Durin::FByteArray BadHash = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	BadHash[48] ^= std::byte{1};
	ExpectError(BadHash, 64, EBinaryEnvelopeError::HeaderHashMismatch);
	Durin::FByteArray ProtectedMutation = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	ProtectedMutation[28] ^= std::byte{1};
	ExpectError(ProtectedMutation, 64, EBinaryEnvelopeError::HeaderHashMismatch);
}

TEST(FBinaryEnvelopeTests, DeterministicPreambleMutationIsBoundedAndStable)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	const Durin::FByteArray Golden = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
	const FValidatedBinaryEnvelope Sentinel{.Preamble = {.FormatId = SecondFormatId}};

	for (size_t Offset = 0; Offset < Golden.size(); ++Offset)
	{
		Durin::FByteArray Mutated = Golden;
		Mutated[Offset] ^= std::byte{0x5a};
		FBinaryEnvelopeDiagnostic FirstDiagnostic;
		FBinaryEnvelopeDiagnostic SecondDiagnostic;
		FValidatedBinaryEnvelope FirstOutput = Sentinel;
		FValidatedBinaryEnvelope SecondOutput = Sentinel;
		const bool bFirst = ValidateBinaryEnvelopeHeader(
			Mutated, Mutated.size(), TestLimits, Registry, FirstOutput, &FirstDiagnostic);
		const bool bSecond = ValidateBinaryEnvelopeHeader(
			Mutated, Mutated.size(), TestLimits, Registry, SecondOutput, &SecondDiagnostic);
		EXPECT_EQ(bFirst, bSecond) << Offset;
		EXPECT_EQ(FirstDiagnostic.Error, SecondDiagnostic.Error) << Offset;
		if (!bFirst)
		{
			EXPECT_EQ(FirstOutput.Preamble.FormatId, SecondFormatId) << Offset;
			EXPECT_EQ(SecondOutput.Preamble.FormatId, SecondFormatId) << Offset;
		}
	}
}

TEST(FBinaryEnvelopeTests, ExtendedFrontMatterMutationAndFinalizationRemainBoundedAndAtomic)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	std::array<std::byte, 192> FormatHeader{};
	for (size_t Index = 0; Index < FormatHeader.size(); ++Index)
		FormatHeader[Index] = static_cast<std::byte>((Index * 37 + 11) & 0xff);
	const Durin::FByteArray Golden = ReferenceEncode(
		FirstFormatId, 3, 1, FormatHeader, 512);
	uint64 State = 0x9e3779b97f4a7c15ull;
	for (size_t Mutation = 0; Mutation < 512; ++Mutation)
	{
		State ^= State << 7;
		State ^= State >> 9;
		State ^= State << 8;
		Durin::FByteArray Bytes = Golden;
		const size_t Offset = static_cast<size_t>(State % Bytes.size());
		Bytes[Offset] ^= static_cast<std::byte>((State >> 24) | 1);
		FValidatedBinaryEnvelope First;
		FValidatedBinaryEnvelope Second;
		FBinaryEnvelopeDiagnostic FirstDiagnostic;
		FBinaryEnvelopeDiagnostic SecondDiagnostic;
		const bool bFirst = ValidateBinaryEnvelopeHeader(
			Bytes, 512, TestLimits, Registry, First, &FirstDiagnostic);
		const bool bSecond = ValidateBinaryEnvelopeHeader(
			Bytes, 512, TestLimits, Registry, Second, &SecondDiagnostic);
		EXPECT_EQ(bFirst, bSecond);
		EXPECT_EQ(FirstDiagnostic.Error, SecondDiagnostic.Error);
	}

	Durin::FByteArray FailedFinalization = Golden;
	ReferenceWrite<uint64>(FailedFinalization, 40, 513);
	const Durin::FByteArray Sentinel = FailedFinalization;
	FBinaryEnvelopeDiagnostic Diagnostic;
	EXPECT_FALSE(FinalizeBinaryEnvelopeHeader(
		FailedFinalization, 512, TestLimits, &Diagnostic));
	EXPECT_EQ(FailedFinalization, Sentinel);
}

TEST(FBinaryEnvelopeTests, HeaderValidationCostIsBoundedForSmallAndMaximumPolicySamples)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	const Durin::FByteArray Small = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
	const Durin::FByteArray Maximum = ReferenceEncode(
		FirstFormatId, 3, 1, Durin::FByteArray(4096 - 64), 4096);

	auto Measure = [&](std::span<const std::byte> Bytes, uint64 PhysicalBytes) {
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
