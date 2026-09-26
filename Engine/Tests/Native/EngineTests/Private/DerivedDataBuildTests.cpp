#include <gtest/gtest.h>

#include "DerivedDataBuild.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <limits>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;

	auto Function() -> FBuildFunctionDescriptor
	{
		return {"Fixture.Copy", 1, 1, "Fixture.Bytes", 1,
			FCacheBucket::FromString("BuildFixture/Objects")};
	}
	auto Input(FXxHash128 Identity = FXxHash128::HashBuffer("source")) -> FBuildInputReference
	{
		return {"Source", Identity, "Fixture.Content", 1, "Fixture.Bytes", 1};
	}
	auto Definition() -> FBuildDefinition
	{
		return FBuildDefinition::TryCreate(Function(), {{"Enabled", true}}, {Input()}).value();
	}

	struct FFixtureError
	{
		std::optional<EBuildFailure> Framework;
		std::string Cause;
	};
	struct FFixtureAdapter
	{
		using FProduct = std::unique_ptr<FByteBuffer>;
		using FError = FFixtureError;
		FBuildFunctionDescriptor Descriptor = Function();
		std::vector<FBuildInputReference> Inputs{Input()};
		FSharedByteBuffer Source = FSharedByteBuffer::Copy(std::as_bytes(std::span("source", 6)));
		uint32 Resolves = 0, Builds = 0, Decodes = 0, Encodes = 0;
		bool bFailResolve = false, bFailBuild = false, bFailEncode = false;
		bool bCancelDecode = false, bInvalidProduct = false, bInvalidBinding = false;

		auto GetFunction() const -> const FBuildFunctionDescriptor& { return Descriptor; }
		auto GetInputs() const -> std::span<const FBuildInputReference> { return Inputs; }
		auto MakeError(EBuildFailure Failure) const -> FError { return {Failure, "framework"}; }
		auto IsCancelled(const FError& Error) const -> bool { return Error.Framework == EBuildFailure::Cancelled; }
		auto ValidateBindings(const FBuildDefinition&) const -> std::expected<void, FError>
		{
			if (bInvalidBinding) return std::unexpected(FError{{}, "snapshot"});
			return {};
		}
		auto Resolve() -> std::expected<FSharedByteBuffer, FError>
		{
			++Resolves;
			if (bFailResolve || FXxHash128::HashBuffer(Source.GetBytes()) != Inputs[0].Identity)
				return std::unexpected(FError{{}, "source"});
			return Source;
		}
		auto Build(const FSharedByteBuffer& Prepared) -> std::expected<FProduct, FError>
		{
			++Builds;
			if (bFailBuild) return std::unexpected(FError{{}, "recipe"});
			return std::make_unique<FByteBuffer>(Prepared.GetBytes().begin(), Prepared.GetBytes().end());
		}
		auto Validate(const FProduct& Product) const -> std::expected<void, FError>
		{
			if (bInvalidProduct || !Product || Product->empty())
				return std::unexpected(FError{{}, "product"});
			return {};
		}
		auto Encode(const FProduct& Product) -> std::expected<FByteBuffer, FError>
		{
			++Encodes;
			if (bFailEncode) return std::unexpected(FError{{}, "encode"});
			FByteBuffer Bytes{std::byte{0xd1}};
			Bytes.insert(Bytes.end(), Product->begin(), Product->end());
			return Bytes;
		}
		auto Decode(const FSharedByteBuffer& Bytes) -> std::expected<FProduct, FError>
		{
			++Decodes;
			if (bCancelDecode) return std::unexpected(MakeError(EBuildFailure::Cancelled));
			if (Bytes.IsEmpty() || Bytes.GetBytes()[0] != std::byte{0xd1})
				return std::unexpected(FError{{}, "decode"});
			return std::make_unique<FByteBuffer>(Bytes.GetBytes().begin() + 1, Bytes.GetBytes().end());
		}
	};

	class FDerivedDataBuildTests : public testing::Test
	{
	protected:
		std::string PreviousRoot;
		std::filesystem::path Root;
		FBuildExecutionPolicy Policy{.MaximumValueBytes = 1024};
		TBuildObservations<FFixtureError> Observations;
		auto SetUp() -> void override
		{
			PreviousRoot = FPaths::DerivedDataCacheDir();
			Root = Testing::CreateTestFixtureDirectory("DerivedDataBuild");
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}
		auto TearDown() -> void override
		{
			FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
			Testing::RemoveTestWorkDirectory(Root);
		}
	};
}

TEST(FBuildDefinitionTests, CanonicalOrderingTypesAndNormalizedFloats)
{
	auto A = FBuildDefinition::TryCreate(Function(), {{"Z", uint64{9}}, {"A", -0.0f}}, {Input()});
	auto B = FBuildDefinition::TryCreate(Function(), {{"A", 0.0f}, {"Z", uint64{9}}}, {Input()});
	ASSERT_TRUE(A); ASSERT_TRUE(B);
	EXPECT_EQ(A->GetKey(), B->GetKey());
	EXPECT_TRUE(std::ranges::equal(A->GetCanonicalBytes(), B->GetCanonicalBytes()));
	EXPECT_EQ(A->GetConstants()[0].Name, "A");
	auto DifferentType = FBuildDefinition::TryCreate(Function(), {{"A", uint64{0}}, {"Z", uint64{9}}}, {Input()});
	ASSERT_TRUE(DifferentType);
	EXPECT_NE(A->GetKey(), DifferentType->GetKey());
	// Integer bytes use little endian, regardless of host ABI.
	auto Integer = FBuildDefinition::TryCreate(Function(), {{"Value", uint64{0x0102030405060708}}}, {});
	ASSERT_TRUE(Integer);
	const auto Bytes = Integer->GetCanonicalBytes();
	ASSERT_GE(Bytes.size(), 12u);
	for (size_t Index = 0; Index < 8; ++Index)
		EXPECT_EQ(Bytes[Bytes.size() - 12 + Index], static_cast<std::byte>(8 - Index));
}

TEST(FBuildDefinitionTests, EveryFunctionAndInputIdentityFieldInvalidatesTheKey)
{
	const auto Original = Definition();
	for (uint32 Field = 0; Field < 6; ++Field)
	{
		auto Changed = Function();
		switch (Field)
		{
		case 0: Changed.Name += "Other"; break;
		case 1: ++Changed.Version; break;
		case 2: ++Changed.ConstantsSchema; break;
		case 3: Changed.OutputType += "Other"; break;
		case 4: ++Changed.OutputSchema; break;
		case 5: Changed.Bucket = FCacheBucket::FromString("BuildFixture/Other"); break;
		}
		auto Built = FBuildDefinition::TryCreate(Changed, {{"Enabled", true}}, {Input()});
		ASSERT_TRUE(Built);
		EXPECT_NE(Built->GetKey(), Original.GetKey());
	}
	for (uint32 Field = 0; Field < 6; ++Field)
	{
		auto Changed = Input();
		switch (Field)
		{
		case 0: Changed.Name += "Other"; break;
		case 1: ++Changed.Identity.HashLow; break;
		case 2: Changed.IdentityScheme += "Other"; break;
		case 3: ++Changed.IdentityVersion; break;
		case 4: Changed.Representation += "Other"; break;
		case 5: ++Changed.RepresentationVersion; break;
		}
		auto Built = FBuildDefinition::TryCreate(Function(), {{"Enabled", true}}, {Changed});
		ASSERT_TRUE(Built);
		EXPECT_NE(Built->GetKey(), Original.GetKey());
	}
	auto ChangedConstant = FBuildDefinition::TryCreate(Function(), {{"Enabled", false}}, {Input()});
	ASSERT_TRUE(ChangedConstant);
	EXPECT_NE(ChangedConstant->GetKey(), Original.GetKey());
}

TEST(FBuildDefinitionTests, RejectsAmbiguousUnboundedAndNonFiniteDefinitions)
{
	EXPECT_FALSE(FBuildDefinition::TryCreate({}, {}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {{"A", true}, {"A", false}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {}, {Input(), Input()}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {}, {Input({})}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {{"../path", true}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {{"A", std::numeric_limits<float>::infinity()}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {{"A", std::numeric_limits<float>::quiet_NaN()}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate(Function(), {{"A", std::string(1024 * 1024 + 1, 'x')}}, {}));
}

TEST_F(FDerivedDataBuildTests, WarmHitNeverResolvesSourceAndColdBuildDoesNotDecodeItsOutput)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	auto Cold = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_TRUE(Cold);
	EXPECT_EQ(Adapter.Resolves, 1u); EXPECT_EQ(Adapter.Builds, 1u); EXPECT_EQ(Adapter.Decodes, 0u);
	EXPECT_EQ(Observations.Origin, EBuildOrigin::Rebuilt);
	EXPECT_EQ(Observations.WrittenBytes, 7u);
	EXPECT_FALSE(Observations.ReadError); // A normal miss is not an error diagnostic.
	Adapter.bFailResolve = true;
	auto Warm = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_TRUE(Warm);
	EXPECT_EQ(**Warm, **Cold);
	EXPECT_EQ(Observations.Origin, EBuildOrigin::CacheHit);
	EXPECT_EQ(Adapter.Resolves, 1u); EXPECT_EQ(Adapter.Builds, 1u); EXPECT_EQ(Adapter.Decodes, 1u);
	EXPECT_FALSE(Observations.ReadError);
	EXPECT_EQ(Observations.WrittenBytes, 0u);
}

TEST_F(FDerivedDataBuildTests, RejectsFunctionAndInputMismatchBeforeReadingCache)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	++Adapter.Descriptor.Version;
	auto Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Framework, EBuildFailure::FunctionMismatch);
	Adapter.Descriptor = Function();
	Adapter.Inputs[0].Identity.HashLow++;
	Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Framework, EBuildFailure::InputMismatch);
	Adapter.Inputs = {Input()}; Adapter.bInvalidBinding = true;
	Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "snapshot");
	EXPECT_EQ(Adapter.Decodes, 0u);
}

TEST_F(FDerivedDataBuildTests, CorruptCacheRebuildsButDecodeCancellationDoesNot)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	const FByteBuffer Corrupt{std::byte{0}};
	ASSERT_TRUE(GetCache().Put({Frozen.GetKey(), Corrupt, 1024}));
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	ASSERT_TRUE(Observations.DecodeError); EXPECT_EQ(Observations.DecodeError->Cause, "decode");
	EXPECT_EQ(Adapter.Builds, 1u);
	Adapter.bCancelDecode = true;
	auto Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_TRUE(Adapter.IsCancelled(Result.error()));
	EXPECT_EQ(Adapter.Builds, 1u); EXPECT_EQ(Adapter.Resolves, 1u);
}

TEST_F(FDerivedDataBuildTests, FailedStorageAndEncodingRetainUsableTypedProducts)
{
	FFixtureAdapter Adapter;
	Policy.MaximumValueBytes = 1;
	auto Result = ExecuteBuild(Definition(), Adapter, Policy, {}, Observations);
	ASSERT_TRUE(Result); ASSERT_TRUE(Observations.WriteError);
	EXPECT_EQ(Observations.WriteError->Code, ECacheError::ValueTooLarge);
	Policy.MaximumValueBytes = 1024;
	Adapter.bFailEncode = true;
	Result = ExecuteBuild(Definition(), Adapter, Policy, {}, Observations);
	ASSERT_TRUE(Result); ASSERT_TRUE(Observations.EncodeError);
	EXPECT_EQ(Observations.EncodeError->Cause, "encode");
	Policy.bFailOnEncodeError = true;
	Result = ExecuteBuild(Definition(), Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "encode");
}

TEST_F(FDerivedDataBuildTests, SourceAndRecipeFailuresNeverPersistPartialOutput)
{
	const auto Frozen = Definition();
	FFixtureAdapter Adapter;
	Adapter.bFailResolve = true;
	auto Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "source");
	EXPECT_EQ(Adapter.Builds, 0u);
	Adapter.bFailResolve = false; Adapter.bFailBuild = true;
	Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "recipe");
	Adapter.bFailBuild = false; Adapter.bInvalidProduct = true;
	Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "product");
	EXPECT_EQ(Adapter.Encodes, 0u);
	EXPECT_FALSE(GetCache().Get({Frozen.GetKey(), 1024}));
}

TEST_F(FDerivedDataBuildTests, CancellationAtEveryColdCheckpointDiscardsTheResult)
{
	for (const auto Phase : {EBuildPhase::Lookup, EBuildPhase::Resolve, EBuildPhase::Build,
		EBuildPhase::Validate, EBuildPhase::Encode, EBuildPhase::Store})
	{
		FFixtureAdapter Adapter;
		bool bCancel = false;
		const FBuildExecutionContext Context{
			.ShouldCancel = [&] { return bCancel; },
			.OnPhase = [&](EBuildPhase Current) { bCancel = Current == Phase; }};
		auto Result = ExecuteBuild(Definition(), Adapter, Policy, Context, Observations);
		ASSERT_FALSE(Result); EXPECT_TRUE(Adapter.IsCancelled(Result.error()));
		EXPECT_FALSE(GetCache().Get({Definition().GetKey(), 1024}));
	}
}

TEST_F(FDerivedDataBuildTests, PolicyDoesNotChangeIdentityAndNoWriteAvoidsEncoding)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	Policy.bForceRebuild = true; Policy.bWriteCache = false;
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	EXPECT_EQ(Adapter.Builds, 2u); EXPECT_EQ(Adapter.Decodes, 0u); EXPECT_EQ(Adapter.Encodes, 1u);
	EXPECT_EQ(Observations.ReadBytes, 0u); EXPECT_EQ(Observations.WrittenBytes, 0u);
	Policy.bForceRebuild = false;
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	EXPECT_EQ(Observations.Origin, EBuildOrigin::CacheHit);
}

TEST_F(FDerivedDataBuildTests, ChangedSourceCannotBuildUnderCapturedIdentity)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	Adapter.Source = FSharedByteBuffer::Copy(std::as_bytes(std::span("edited", 6)));
	auto Result = ExecuteBuild(Frozen, Adapter, Policy, {}, Observations);
	ASSERT_FALSE(Result); EXPECT_EQ(Result.error().Cause, "source");
	EXPECT_EQ(Adapter.Builds, 0u);
	EXPECT_FALSE(GetCache().Get({Frozen.GetKey(), 1024}));
}

TEST_F(FDerivedDataBuildTests, LateCancellationCanLeaveAtomicCacheButNeverReturnsAProduct)
{
	FFixtureAdapter Adapter;
	const auto Frozen = Definition();
	bool bAtStore = false;
	uint32 StoreChecks = 0;
	const FBuildExecutionContext Context{
		.ShouldCancel = [&] { return bAtStore && ++StoreChecks > 1; },
		.OnPhase = [&](EBuildPhase Phase) { bAtStore = Phase == EBuildPhase::Store; }};
	auto Result = ExecuteBuild(Frozen, Adapter, Policy, Context, Observations);
	ASSERT_FALSE(Result); EXPECT_TRUE(Adapter.IsCancelled(Result.error()));
	EXPECT_TRUE(GetCache().Get({Frozen.GetKey(), 1024}));
	Adapter.bFailResolve = true;
	ASSERT_TRUE(ExecuteBuild(Frozen, Adapter, Policy, {}, Observations));
	EXPECT_EQ(Observations.Origin, EBuildOrigin::CacheHit);
}

TEST(FBuildDefinitionTests, MatchesLargeUnorderedBindingsAndRejectsDuplicatesOrChangedRepresentations)
{
	std::vector<FBuildInputReference> Inputs;
	for (size_t Index = 0; Index < 4096; ++Index)
	{
		auto Value = Input(); Value.Name = std::format("Source{:04}", Index);
		Inputs.push_back(std::move(Value));
	}
	auto Frozen = FBuildDefinition::TryCreate(Function(), {}, Inputs);
	ASSERT_TRUE(Frozen);
	EXPECT_TRUE(Frozen->MatchesInputs(Inputs));
	std::ranges::reverse(Inputs);
	EXPECT_TRUE(Frozen->MatchesInputs(Inputs));
	Inputs[20].RepresentationVersion++;
	EXPECT_FALSE(Frozen->MatchesInputs(Inputs));
	Inputs[20].RepresentationVersion--;
	Inputs[20] = Inputs[21];
	EXPECT_FALSE(Frozen->MatchesInputs(Inputs));
	Inputs.pop_back();
	EXPECT_FALSE(Frozen->MatchesInputs(Inputs));
}

TEST(FBuildDefinitionTests, IssueVisitorKeepsStagesAndCausesWithoutReportingNormalMisses)
{
	TBuildObservations<FFixtureError> Observations;
	Observations.ReadError = FCacheError{ECacheError::Miss, "miss"};
	Observations.DecodeError = FFixtureError{{}, "decode cause"};
	Observations.EncodeError = FFixtureError{{}, "encode cause"};
	Observations.WriteError = FCacheError{ECacheError::StorageFailure, "store cause"};
	std::vector<EBuildPhase> Phases;
	std::vector<std::string> Causes;
	VisitBuildIssues(Observations, [&](EBuildPhase Phase, const auto& Error) {
		Phases.push_back(Phase);
		if constexpr (std::is_same_v<std::decay_t<decltype(Error)>, FCacheError>) Causes.push_back(Error.Diagnostic);
		else Causes.push_back(Error.Cause);
	});
	EXPECT_EQ(Phases, (std::vector{EBuildPhase::Decode, EBuildPhase::Encode, EBuildPhase::Store}));
	EXPECT_EQ(Causes, (std::vector<std::string>{"decode cause", "encode cause", "store cause"}));
}
