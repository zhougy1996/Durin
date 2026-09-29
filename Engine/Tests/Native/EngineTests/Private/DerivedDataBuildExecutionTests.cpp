#include "DerivedDataBuildSession.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Ref() -> FBuildInputReference { return {"Source", FXxHash128::HashBuffer("captured"), "Fixture", 1, "Bytes", 1}; }
	auto Definition() -> FBuildDefinition { FBuildDefinitionBuilder B("Execution.Fixture"); B.AddInput("Source", "Captured"); return std::move(B).Build().value(); }
	struct FResolver final : IBuildInputResolver
	{
		mutable uint32 Describes = 0, Resolves = 0;
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override { ++Describes; return std::vector{Ref()}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInput>, FBuildInputError> override { ++Resolves; return std::vector<FBuildInput>{{.Identity = Ref(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(32, std::byte{3}))}}}}; }
	};
	struct FFunction final : IBuildFunction
	{
		mutable uint32 Builds = 0; bool Fail = false, Throw = false, Log = false;
		auto GetName() const -> std::string_view override { return "Execution.Fixture"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& C) const -> void override { C.SetConstantsSchema(1); C.SetOutput("Fixture.Output", 1); C.SetCacheBucket(FCacheBucket::FromString("Execution")); }
		auto Build(FBuildContext& C) const -> void override { ++Builds; if (Throw) throw std::runtime_error("fixture exception"); if (Fail) return C.AddError("deterministic failure"), void(); const auto* I = C.FindInput("Source"); if (!I) return C.AddError("missing input"), void(); C.AddValue(FValueId::FromName("Data"), I->Values[0].Data); if (Log) C.AddLog("Fixture", EBuildLogSeverity::Warning, "transient warning"); }
	};
	struct FMemoryCache final : ICache
	{
		mutable uint32 Gets = 0, Puts = 0;
		mutable bool Corrupt = false;
		mutable std::optional<FCacheRecord> Stored;
		auto Get(const FCacheGetRequest&) const -> FCacheGetResult override
		{
			++Gets;
			if (Corrupt)
			{
				Corrupt = false;
				return std::unexpected(FCacheError{ECacheError::Corrupt, "fixture corruption"});
			}
			return Stored;
		}
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
		{
			++Puts; Stored = Request.Record; return {};
		}
	};
	struct FHarness
	{
		std::shared_ptr<FResolver> Resolver = std::make_shared<FResolver>(); std::shared_ptr<FFunction> Function = std::make_shared<FFunction>(); std::shared_ptr<FMemoryCache> Cache = std::make_shared<FMemoryCache>();
		std::shared_ptr<IBuild> Service; std::shared_ptr<FBuildSession> Session;
		FHarness()
		{
			FBuildServiceOptions O; O.Cache = Cache;
			Service = CreateBuild(std::move(O)); Service->Register(Function).value(); Session = Service->CreateSession().value();
		}
		auto Run(FBuildRequestOptions O = {}) -> FBuildCompleteParams
		{
			auto DefinitionValue = Definition(); auto Inputs = std::move(FBuildInputsBuilder(DefinitionValue.GetSources(), Resolver)).Build().value(); std::optional<FBuildCompleteParams> Result;
			Session->Build(std::move(DefinitionValue), [&](auto V) { Result = std::move(V); }, std::move(Inputs), std::move(O)).value(); return std::move(*Result);
		}
	};
}

TEST(FBuildExecutionTests, ColdBuildPersistsAndWarmHitAvoidsResolve)
{
	FHarness H; auto Cold = H.Run(); ASSERT_EQ(Cold.GetStatus(), EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
	auto Warm = H.Run(); ASSERT_EQ(Warm.GetStatus(), EStatus::Ok); EXPECT_TRUE(HasBuildStatus(Warm.GetBuildStatus(), EBuildStatus::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Resolver->Resolves, 1u);
}

TEST(FBuildExecutionTests, CorruptRecordFallsBackOnce)
{
	FHarness H; H.Cache->Corrupt = true; auto Result = H.Run(); EXPECT_EQ(Result.GetStatus(), EStatus::Ok); EXPECT_EQ(H.Function->Builds, 1u); EXPECT_EQ(H.Cache->Gets, 1u); EXPECT_EQ(H.Cache->Puts, 1u);
}

TEST(FBuildExecutionTests, TransientLogsPreventCacheStorage)
{
	FHarness H; H.Function->Log = true; auto Result = H.Run(); ASSERT_EQ(Result.GetStatus(), EStatus::Ok); ASSERT_NE(Result.GetOutput(), nullptr); EXPECT_TRUE(Result.GetOutput()->HasLogs()); EXPECT_EQ(H.Cache->Puts, 0u);
}

TEST(FBuildExecutionTests, DeterministicErrorIsPersistedAndReplayed)
{
	FHarness H; H.Function->Fail = true; auto Cold = H.Run(); ASSERT_EQ(Cold.GetStatus(), EStatus::Error); ASSERT_NE(Cold.GetOutput(), nullptr); EXPECT_TRUE(Cold.GetOutput()->HasError()); EXPECT_EQ(H.Cache->Puts, 1u);
	H.Function->Fail = false; auto Warm = H.Run(); EXPECT_EQ(Warm.GetStatus(), EStatus::Error); EXPECT_TRUE(HasBuildStatus(Warm.GetBuildStatus(), EBuildStatus::CacheQueryHit)); EXPECT_EQ(H.Function->Builds, 1u);
}

TEST(FBuildExecutionTests, CancellationRemainsDistinct)
{
	FHarness H; FBuildRequestOptions O; O.Cancellation = FBuildCancellation([] { return true; }); auto Result = H.Run(std::move(O)); EXPECT_EQ(Result.GetStatus(), EStatus::Canceled); EXPECT_EQ(Result.GetOutput(), nullptr);
}

TEST(FBuildExecutionTests, ProducerExceptionIsInfrastructureFailureAndIsNotCached)
{
	FHarness H; H.Function->Throw = true; auto Result = H.Run(); EXPECT_EQ(Result.GetStatus(), EStatus::Error); EXPECT_EQ(Result.GetOutput(), nullptr); EXPECT_EQ(H.Cache->Puts, 0u);
}
