#include "BinaryEnvelopeTestSupport.h"

TEST(FBinaryEnvelopeTests, FieldTableAndIndependentGoldensFreezeTheWireContract)
{
	EXPECT_EQ(BinaryEnvelopeHeaderVersion, 1);
	EXPECT_EQ(BinaryEnvelopePreambleBytes, 64);

	const Durin::FByteBuffer Minimal = ReferenceEncode(FirstFormatId, 2, 0, {}, 64);
	const std::array FormatHeader{
		std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
		std::byte{0x31}, std::byte{0x41}, std::byte{0x59}};
	const Durin::FByteBuffer Extended = ReferenceEncode(
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
	const Durin::FByteBuffer Reference = ReferenceEncode(FirstFormatId, 3, 1, FormatHeader, 96);
	Durin::FByteBuffer Production(Reference.size(), std::byte{0x7b});
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
	const Durin::FByteBuffer Bytes = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
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
	Durin::FByteBuffer Corrupt = Bytes;
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
	auto ExpectError = [&](const Durin::FByteBuffer& Bytes, uint64 PhysicalBytes,
		const FBinaryEnvelopeLimits& Limits, EBinaryEnvelopeError Error) {
		Output = Sentinel;
		EXPECT_FALSE(ParseBinaryEnvelopePrefix(Bytes, PhysicalBytes, Limits, Output, &Diagnostic));
		EXPECT_EQ(Diagnostic.Error, Error);
		EXPECT_EQ(Output.FormatId, Sentinel.FormatId);
	};

	Durin::FByteBuffer Bytes = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
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

	auto ExpectError = [&](const Durin::FByteBuffer& Bytes, uint64 PhysicalBytes,
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

	Durin::FByteBuffer BadHash = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	BadHash[48] ^= std::byte{1};
	ExpectError(BadHash, 64, EBinaryEnvelopeError::HeaderHashMismatch);
	Durin::FByteBuffer ProtectedMutation = ReferenceEncode(FirstFormatId, 3, 0, {}, 64);
	ProtectedMutation[28] ^= std::byte{1};
	ExpectError(ProtectedMutation, 64, EBinaryEnvelopeError::HeaderHashMismatch);
}

TEST(FBinaryEnvelopeTests, DeterministicPreambleMutationIsBoundedAndStable)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	const Durin::FByteBuffer Golden = ReferenceEncode(FirstFormatId, 3, 1, {}, 64);
	const FValidatedBinaryEnvelope Sentinel{.Preamble = {.FormatId = SecondFormatId}};

	for (size_t Offset = 0; Offset < Golden.size(); ++Offset)
	{
		Durin::FByteBuffer Mutated = Golden;
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
	const Durin::FByteBuffer Golden = ReferenceEncode(
		FirstFormatId, 3, 1, FormatHeader, 512);
	uint64 State = 0x9e3779b97f4a7c15ull;
	for (size_t Mutation = 0; Mutation < 512; ++Mutation)
	{
		State ^= State << 7;
		State ^= State >> 9;
		State ^= State << 8;
		Durin::FByteBuffer Bytes = Golden;
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

	Durin::FByteBuffer FailedFinalization = Golden;
	ReferenceWrite<uint64>(FailedFinalization, 40, 513);
	const Durin::FByteBuffer Sentinel = FailedFinalization;
	FBinaryEnvelopeDiagnostic Diagnostic;
	EXPECT_FALSE(FinalizeBinaryEnvelopeHeader(
		FailedFinalization, 512, TestLimits, &Diagnostic));
	EXPECT_EQ(FailedFinalization, Sentinel);
}

TEST(FBinaryEnvelopeTests, ValidatesSmallAndMaximumPolicyHeaders)
{
	const std::array Descriptors{MakeDescriptor()};
	const FBinaryFormatRegistry Registry = MakeRegistry(Descriptors);
	for (const size_t HeaderBytes : {size_t{64}, size_t{4096}})
	{
		const auto Bytes = ReferenceEncode(
			FirstFormatId, 3, 1, Durin::FByteBuffer(HeaderBytes - 64), HeaderBytes);
		FValidatedBinaryEnvelope Output;
		EXPECT_TRUE(ValidateBinaryEnvelopeHeader(
			Bytes, HeaderBytes, TestLimits, Registry, Output));
	}
}
