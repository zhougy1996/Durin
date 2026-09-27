#include "DerivedDataBuildExecution.h"
#include <gtest/gtest.h>
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "AssetCacheLogTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	struct FResolver final : IBuildInputResolver
	{
		mutable uint32 Describes = 0, Resolves = 0;
		bool WrongIdentity = false, Fail = false;
		std::vector<FBuildValue> ExtraValues;
		FSharedByteBuffer Bytes = FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{7}));
		FXxHash128 CapturedIdentity = FXxHash128::HashBuffer(Bytes.GetBytes());
		auto Identity() const -> FBuildInputReference { return {"Source", CapturedIdentity, "Fixture.Hash", 1, "Fixture.Bytes", 1}; }
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildError> override
		{ ++Describes; return std::vector{Identity()}; }
		auto Resolve(std::span<const FBuildInputReference> Expected, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInput>, FBuildError> override
		{
			++Resolves;
			if (Fail || Expected[0] != Identity() || FXxHash128::HashBuffer(Bytes.GetBytes()) != CapturedIdentity) return std::unexpected(FBuildError{.Description = "Source unavailable"});
			auto Id = Identity(); if (WrongIdentity) ++Id.RepresentationVersion;
			FBuildInput Input{.Identity = Id, .Values = {{"Data", Bytes}}};
			Input.Values.insert(Input.Values.end(), ExtraValues.begin(), ExtraValues.end());
			return std::vector{std::move(Input)};
		}
	};
	struct FFunction final : IBuildFunction
	{
		mutable uint32 Builds = 0, Validations = 0;
		mutable uint64 Budget = 0;
		bool Reject = false, FailBuild = false;
		bool CollapseInput = false;
		mutable size_t InputValues = 0;
		bool* CancelDuringValidation = nullptr;
		auto GetDescriptor() const -> FBuildFunctionDescriptor override
		{ return {"Fixture.Copy", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("ExecutionFixture")}; }
		auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildError> override
		{
			++Builds; Budget = Context.GetMaximumWorkingSetBytes();
			if (FailBuild) return std::unexpected(FBuildError{.Category = EBuildErrorCategory::ProducerFailure,
				.Description = std::string(5000, 'x'), .ProducerCode = 17, .DiagnosticIdentity = FXxHash128::HashBuffer("cause")});
			const auto& Values = Context.GetInputs()[0].Values;
			InputValues = Values.size();
			EXPECT_TRUE(std::ranges::is_sorted(Values, {}, &FBuildValue::Id));
			auto Output = FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1,
				.Values = CollapseInput ? std::vector{Values.front()} : Values});
			return std::move(Output.value());
		}
		auto Validate(const FBuildAction&, const FBuildOutput& Output, const FBuildCancellation&) const -> std::expected<void, FBuildError> override
		{
			++Validations;
			if (CancelDuringValidation) *CancelDuringValidation = true;
			if (Reject || !Output.FindValue("Data")) return std::unexpected(FBuildError{.Category = EBuildErrorCategory::InvalidOutput, .Description = "Missing Data"});
			return {};
		}
	};
	struct FHarness
	{
		FResolver Resolver;
		std::shared_ptr<FFunction> Function = std::make_shared<FFunction>();
		FBuildRegistrySnapshot Registry;
		FBuildDefinition Definition = FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", "Capture"}}).value();
		FBuildRequestPolicy Policy;
		FBuildCacheOperations Cache;
		FBuildRunObserver Observer;
		FSharedByteBuffer Stored;
		FCacheKey StoredKey;
		uint32 Gets = 0, Puts = 0, Issues = 0, Hits = 0;
		std::array<uint32, static_cast<size_t>(EBuildSessionPhase::Count)> Phases{};
		bool Cancel = false;
		FHarness()
		{
			FBuildRegistry Mutable; Mutable.Register(Function).value(); Registry = Mutable.Freeze().value();
			Cache.Get = [&](const FCacheGetRequest& Request) -> FCacheGetResult {
				++Gets;
				if (Request.Key != StoredKey || Stored.IsEmpty()) return std::unexpected(FCacheError{ECacheError::Miss, "miss"});
				return Stored;
			};
			Cache.Put = [&](const FCachePutRequest& Request) -> FCachePutResult {
				++Puts; StoredKey = Request.Key; Stored = FSharedByteBuffer::Copy(Request.Value); return {};
			};
			Observer.OnCacheIssue = [&](const auto&, auto, const auto&) { ++Issues; };
			Observer.OnCacheHit = [&] { ++Hits; };
			Observer.OnPhase = [&](auto Phase) { ++Phases[static_cast<size_t>(Phase)]; };
		}
		auto Run() -> FBuildCompletion { return ExecuteBuildRequest(Definition, Registry, Resolver, Policy, FBuildCancellation([&] { return Cancel; }), Cache, Observer); }
	};
}

TEST(FBuildExecutionTests, DefaultDiagnosticsKeepStageKeyAndBoundedCauseWithoutLoggingMisses)
{
	FHarness H;
	H.Observer.OnCacheIssue = {};
	FCacheLogCapture Capture;
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	EXPECT_TRUE(Capture.empty());
	const auto Key = H.StoredKey.ToString();
	H.Policy.ForceRebuild = true;
	H.Cache.Encode = [](const FCacheRecord&, uint64) -> std::expected<FSharedByteBuffer, FCacheError> {
		return std::unexpected(FCacheError{ECacheError::StorageFailure,
			"specific encoder failure " + std::string(5000, 'x')});
	};
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	ASSERT_EQ(Capture.size(), 1u);
	const auto Record = Capture.front();
	EXPECT_NE(Record.Message.find("cache encode"), std::string::npos);
	EXPECT_NE(Record.Message.find(Key), std::string::npos);
	EXPECT_NE(Record.Message.find("specific encoder failure"), std::string::npos);
	EXPECT_LT(Record.Message.size(), FBuildError::MaximumDescriptionBytes + 128);
}

TEST(FBuildExecutionTests, ColdWarmAndDisabledWritesRetainBlocksAndSkipWork)
{
	FHarness H; H.Policy.Compress = true;
	auto Cold = H.Run(); ASSERT_EQ(Cold.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(Cold.Output->FindValue("Data")->Data.data(), H.Resolver.Bytes.data());
	ASSERT_EQ(H.Puts, 1u); EXPECT_EQ(H.Issues, 0u);
	auto Warm = H.Run(); ASSERT_EQ(Warm.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Hits, 1u); EXPECT_EQ(H.Resolver.Describes, 2u); EXPECT_EQ(H.Resolver.Resolves, 1u); EXPECT_EQ(H.Function->Builds, 1u);
	H.Policy.ForceRebuild = true; H.Policy.WriteCache = false;
	H.Phases.fill(0);
	auto NoWrite = H.Run(); ASSERT_EQ(NoWrite.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Gets, 2u); EXPECT_EQ(H.Puts, 1u);
	for (auto Phase : {EBuildSessionPhase::Record, EBuildSessionPhase::Encode, EBuildSessionPhase::Compress, EBuildSessionPhase::Store})
		EXPECT_EQ(H.Phases[static_cast<size_t>(Phase)], 0u);
	EXPECT_EQ(NoWrite.Output->FindValue("Data")->Data.data(), H.Resolver.Bytes.data());
}

TEST(FBuildExecutionTests, EachOptionalPersistenceFailurePreservesOutputAndStopsLaterOperations)
{
	for (auto FailurePhase : {EBuildSessionPhase::Record, EBuildSessionPhase::Encode, EBuildSessionPhase::Compress, EBuildSessionPhase::Store})
	{
		FHarness H; H.Policy.Compress = true;
		auto Error = [] { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Injected"}); };
		if (FailurePhase == EBuildSessionPhase::Record) H.Cache.MakeRecord = [&](const auto&, const auto&, auto) -> std::expected<FCacheRecord, FCacheError> { return Error(); };
		if (FailurePhase == EBuildSessionPhase::Encode) H.Cache.Encode = [&](const auto&, auto) -> FCacheGetResult { return Error(); };
		if (FailurePhase == EBuildSessionPhase::Compress) H.Cache.Compress = [&](const auto&, auto) -> FCacheGetResult { return Error(); };
		if (FailurePhase == EBuildSessionPhase::Store) H.Cache.Put = [&](const auto&) -> FCachePutResult { return Error(); };
		auto Result = H.Run(); ASSERT_EQ(Result.Status, EBuildStatus::Succeeded);
		EXPECT_EQ(Result.Output->FindValue("Data")->Data.data(), H.Resolver.Bytes.data());
		EXPECT_EQ(H.Issues, 1u); EXPECT_EQ(H.Function->Builds, 1u);
		for (size_t Index = static_cast<size_t>(FailurePhase) + 1; Index <= static_cast<size_t>(EBuildSessionPhase::Store); ++Index)
			EXPECT_EQ(H.Phases[Index], 0u);
	}
}

TEST(FBuildExecutionTests, CorruptAndSemanticallyInvalidCacheRebuildOnlyOnce)
{
	for (bool Semantic : {false, true})
	{
		FHarness H; ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
		if (Semantic)
		{
			auto Wrong = FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1, .Values = {{"Other", H.Resolver.Bytes}}});
			H.Stored = FCacheRecord::FromOutput(H.StoredKey, *Wrong)->Encode().value();
		}
		else H.Stored = FSharedByteBuffer::Take(FByteBuffer(32));
		auto Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Succeeded);
		EXPECT_EQ(H.Issues, 1u); EXPECT_EQ(H.Function->Builds, 2u); EXPECT_EQ(H.Resolver.Resolves, 2u);
		H.Function->Reject = true;
		Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Failed);
		EXPECT_FALSE(Result.Output); EXPECT_EQ(H.Function->Builds, 3u); EXPECT_EQ(H.Puts, 2u);
	}
}

TEST(FBuildExecutionTests, CancellationDuringCachedValidationNeverResolvesOrRebuilds)
{
	FHarness H; ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	H.Function->CancelDuringValidation = &H.Cancel;
	auto Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Cancelled);
	EXPECT_FALSE(Result.Output); EXPECT_FALSE(Result.Error);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver.Resolves, 1u); EXPECT_EQ(H.Issues, 0u);
}

TEST(FBuildExecutionTests, CancellationAfterWriteDoesNotPublishOutput)
{
	FHarness H;
	auto Put = H.Cache.Put;
	H.Cache.Put = [&](const auto& Request) { auto Result = Put(Request); H.Cancel = true; return Result; };
	auto Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Cancelled); EXPECT_FALSE(Result.Output);
	EXPECT_EQ(H.Puts, 1u);
	H.Cancel = false; Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Succeeded); EXPECT_EQ(H.Hits, 1u);
}

TEST(FBuildExecutionTests, ResolutionFreshValidationAndProducerFailuresNeverPersist)
{
	for (uint32 Kind = 0; Kind < 4; ++Kind)
	{
		FHarness H;
		H.Resolver.WrongIdentity = Kind == 0; H.Resolver.Fail = Kind == 1;
		H.Function->Reject = Kind == 2; H.Function->FailBuild = Kind == 3;
		auto Result = H.Run(); ASSERT_EQ(Result.Status, EBuildStatus::Failed); ASSERT_TRUE(Result.Error);
		EXPECT_FALSE(Result.Output); EXPECT_EQ(H.Puts, 0u);
		EXPECT_EQ(H.Phases[static_cast<size_t>(EBuildSessionPhase::Record)], 0u);
		if (Kind < 2) EXPECT_EQ(H.Function->Builds, 0u);
		if (Kind == 3)
		{
			EXPECT_EQ(Result.Error->ProducerCode, 17u); EXPECT_EQ(Result.Error->Description.size(), 4096u);
			EXPECT_EQ(Result.Error->DiagnosticIdentity, FXxHash128::HashBuffer("cause"));
		}
	}
}

TEST(FBuildExecutionTests, EveryColdPhaseHonorsCancellationBeforeInvokingItsOperation)
{
	for (size_t Index = 0; Index <= static_cast<size_t>(EBuildSessionPhase::Store); ++Index)
	{
		if (Index == static_cast<size_t>(EBuildSessionPhase::Decode)) continue; // Exercised on the warm path below.
		FHarness H; H.Policy.Compress = true;
		H.Observer.OnPhase = [&](auto Phase) { if (static_cast<size_t>(Phase) == Index) H.Cancel = true; };
		auto Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Cancelled) << Index;
		EXPECT_FALSE(Result.Output); EXPECT_EQ(H.Puts, 0u); EXPECT_EQ(H.Issues, 0u);
	}
	FHarness H; ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	H.Observer.OnPhase = [&](auto Phase) { if (Phase == EBuildSessionPhase::Decode) H.Cancel = true; };
	EXPECT_EQ(H.Run().Status, EBuildStatus::Cancelled);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver.Resolves, 1u);
}

TEST(FBuildExecutionTests, InputOutputAndPersistenceBudgetsHaveDifferentFailureBoundaries)
{
	FHarness Input; Input.Policy.InputLimits.MaximumTotalBytes = 1;
	EXPECT_EQ(Input.Run().Status, EBuildStatus::Failed); EXPECT_EQ(Input.Function->Builds, 0u);
	FHarness Output; Output.Policy.OutputLimits.MaximumTotalBytes = 1;
	EXPECT_EQ(Output.Run().Status, EBuildStatus::Failed); EXPECT_EQ(Output.Function->Builds, 1u); EXPECT_EQ(Output.Puts, 0u);
	FHarness Persistence; Persistence.Policy.PersistenceLimits.MaximumTotalBytes = 1;
	auto Result = Persistence.Run(); ASSERT_EQ(Result.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(Persistence.Issues, 1u); EXPECT_EQ(Persistence.Puts, 0u);
	EXPECT_EQ(Result.Output->FindValue("Data")->Data.data(), Persistence.Resolver.Bytes.data());
}

TEST(FBuildExecutionTests, ReadFailuresRebuildAndPersistenceAllocationFailurePreservesOutput)
{
	FHarness H;
	H.Cache.Get = [](const auto&) -> FCacheGetResult { return std::unexpected(FCacheError{ECacheError::StorageFailure, "read failure"}); };
	H.Cache.Encode = [](const auto&, auto) -> FCacheGetResult { throw std::bad_alloc(); };
	H.Observer.OnCacheIssue = [&](const auto&, auto, const auto&) { ++H.Issues; throw std::bad_alloc(); };
	auto Result = H.Run(); ASSERT_EQ(Result.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Issues, 2u); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Puts, 0u);
	EXPECT_EQ(Result.Output->FindValue("Data")->Data.data(), H.Resolver.Bytes.data());
}

TEST(FBuildExecutionTests, RealFileSystemFailureStillReturnsTheRecipeAllocation)
{
	struct FScope
	{
		std::string Previous = FPaths::DerivedDataCacheDir();
		std::filesystem::path Root = Testing::CreateTestFixtureDirectory("ExecutionPersistence");
		~FScope() { FPaths::SetDerivedDataCacheDirForTests(Previous); Testing::RemoveTestWorkDirectory(Root); }
	} Scope;
	FPaths::SetDerivedDataCacheDirForTests(Scope.Root.generic_string());
	FHarness H; H.Cache = {};
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Hits, 1u); EXPECT_EQ(H.Resolver.Resolves, 1u);
	const auto Blocker = Scope.Root / "blocked";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(H.Resolver.Bytes.GetBytes(), Blocker));
	FPaths::SetDerivedDataCacheDirForTests((Blocker / "cache").generic_string());
	H.Policy.ForceRebuild = true;
	auto Result = H.Run(); ASSERT_EQ(Result.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Issues, 1u); EXPECT_EQ(H.Function->Builds, 2u);
	EXPECT_EQ(Result.Output->FindValue("Data")->Data.data(), H.Resolver.Bytes.data());
}

TEST(FBuildExecutionTests, WarmMetadataDoesNotRequireReadableSourceButMissRejectsChangedContent)
{
	FHarness H; ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	H.Resolver.Bytes = {};
	EXPECT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Resolver.Resolves, 1u); EXPECT_EQ(H.Hits, 1u);
	H.Policy.ForceRebuild = true;
	auto Result = H.Run(); EXPECT_EQ(Result.Status, EBuildStatus::Failed);
	EXPECT_EQ(Result.Error->Phase, EBuildSessionPhase::Resolve);
	EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Puts, 1u);
}


TEST(FDerivedDataBuildExecutionTests, ExecutionReservationReachesFunctionWithoutChangingActionIdentity)
{
	FHarness H;
	H.Policy.ForceRebuild = true;
	H.Policy.MaximumWorkingSetBytes = 1024;
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Function->Budget, 1024u);
	const auto Key = H.StoredKey;
	H.Policy.MaximumWorkingSetBytes = 8192;
	ASSERT_EQ(H.Run().Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Function->Budget, 8192u);
	EXPECT_EQ(H.StoredKey, Key);
}

TEST(FBuildExecutionTests, LargeInputTablesRequireExplicitAdmissionAndKeepOutputBounds)
{
	FHarness H; H.Policy.ReadCache = false; H.Policy.WriteCache = false; H.Function->CollapseInput = true;
	for (uint32 Index = 0; Index < 65536; ++Index)
		H.Resolver.ExtraValues.push_back({"File/" + std::to_string(Index), FSharedByteBuffer{}});
	auto Rejected = H.Run(); EXPECT_EQ(Rejected.Status, EBuildStatus::Failed); EXPECT_EQ(H.Function->Builds, 0u);
	H.Policy.InputLimits.MaximumValues = 65537;
	auto Accepted = H.Run(); ASSERT_EQ(Accepted.Status, EBuildStatus::Succeeded);
	EXPECT_EQ(H.Function->InputValues, 65537u); EXPECT_EQ(Accepted.Output->GetValues().size(), 1u);
	EXPECT_FALSE(FBuildOutput::TryCreate({.Schema = "Fixture.Output", .SchemaVersion = 1, .Values = H.Resolver.ExtraValues},
		{.MaximumValues = FBuildInput::MaximumValues}));
	H.Resolver.ExtraValues[0].Id = "Data";
	EXPECT_EQ(H.Run().Status, EBuildStatus::Failed); EXPECT_EQ(H.Function->Builds, 1u);
	H.Resolver.ExtraValues[0].Id = "invalid id";
	EXPECT_EQ(H.Run().Status, EBuildStatus::Failed); EXPECT_EQ(H.Function->Builds, 1u);
	H.Resolver.ExtraValues[0].Id = "File/0";
	H.Policy.InputLimits.MaximumTotalBytes = H.Resolver.Bytes.size() - 1;
	EXPECT_EQ(H.Run().Status, EBuildStatus::Failed); EXPECT_EQ(H.Function->Builds, 1u);
	H.Policy.InputLimits.MaximumTotalBytes = 8192;
	H.Policy.InputLimits.MaximumValues = FBuildInput::MaximumValues + 1;
	while (H.Resolver.ExtraValues.size() < FBuildInput::MaximumValues)
		H.Resolver.ExtraValues.push_back({"More/" + std::to_string(H.Resolver.ExtraValues.size()), {}});
	EXPECT_EQ(H.Run().Status, EBuildStatus::Failed); EXPECT_EQ(H.Function->Builds, 1u);
}
