#include <gtest/gtest.h>

#include "RendererResourceSlotCache.h"

namespace Durin
{
	namespace
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<int>;
		using FCache = TRendererResourceSlotCache<int, int>;

		auto MakeCacheFailure() -> FRenderResourceCreateError
		{
			return {
				.Category =
					ERenderResourceCreateErrorCategory::GraphicsPipeline,
				.Context = "StaticMeshPipeline",
				.Identity = "test-key",
				.Message = "injected pipeline failure",
				.RetryDependencies =
					EDependency::Shader
					| EDependency::Device
					| EDependency::Manual,
			};
		}

		auto MakeShaderMapFailure() -> FRenderResourceCreateError
		{
			return {
				.Category =
					ERenderResourceCreateErrorCategory::ShaderCompile,
				.Context = "StaticMeshShaderMap",
				.Identity = "test-shader-map",
				.Message = "injected shader compile failure",
				.RetryDependencies =
					EDependency::Shader | EDependency::Manual,
			};
		}

		TEST(
			FRendererResourceSlotCacheTests,
			ShaderMapFailureRecoversForSameIdentityAfterShaderGeneration)
		{
			FCache Cache(EDependency::Shader);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			int Attempts = 0;
			auto& Entry = Cache.FindOrAdd(7);

			EXPECT_EQ(
				Entry.Slot.Resolve(
					Generation,
					[&]() {
						++Attempts;
						return Attempts == 1
							? FResult::Failure(MakeShaderMapFailure())
							: FResult::Success(77);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(
				Entry.Slot.Resolve(
					Generation,
					[&]() {
						++Attempts;
						return FResult::Success(77);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(Attempts, 1);

			Generation.Advance(EDependency::Shader);
			ASSERT_NE(
				Entry.Slot.Resolve(
					Generation,
					[&]() {
						++Attempts;
						return FResult::Success(77);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(*Entry.Slot.GetPayload(), 77);
			EXPECT_EQ(Attempts, 2);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			FailedKeyRetriesAfterRelevantGenerationWithoutPoisoningOtherKey)
		{
			FCache Cache(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			int FirstKeyAttempts = 0;

			auto& FirstEntry = Cache.FindOrAdd(1);
			EXPECT_EQ(
				FirstEntry.Slot.Resolve(
					Generation,
					[&]() {
						++FirstKeyAttempts;
						return FirstKeyAttempts == 1
							? FResult::Failure(MakeCacheFailure())
							: FResult::Success(11);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(
				FirstEntry.Slot.Resolve(
					Generation,
					[&]() {
						++FirstKeyAttempts;
						return FResult::Success(11);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(FirstKeyAttempts, 1);

			auto& SecondEntry = Cache.FindOrAdd(2);
			ASSERT_NE(
				SecondEntry.Slot.Resolve(
					Generation,
					[]() { return FResult::Success(22); },
					Reporter),
				nullptr);
			EXPECT_EQ(*SecondEntry.Slot.GetPayload(), 22);
			EXPECT_EQ(Cache.Num(), 2);

			Generation.Advance(EDependency::Shader);
			auto* StableFirstEntry = Cache.Find(1);
			ASSERT_NE(StableFirstEntry, nullptr);
			ASSERT_NE(
				StableFirstEntry->Slot.Resolve(
					Generation,
					[&]() {
						++FirstKeyAttempts;
						return FResult::Success(11);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(*StableFirstEntry->Slot.GetPayload(), 11);
			EXPECT_EQ(FirstKeyAttempts, 2);
			EXPECT_EQ(*Cache.Find(2)->Slot.GetPayload(), 22);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			LateAggregateFailureRetainsOldCompletePayload)
		{
			struct FPipelineAggregate
			{
				int Solid = 0;
				int Wire = 0;
			};
			using FAggregateResult =
				TRenderResourceCreateResult<FPipelineAggregate>;
			TRendererResourceSlotCache<int, FPipelineAggregate> Cache(
				EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto& Entry = Cache.FindOrAdd(3);

			ASSERT_NE(
				Entry.Slot.Resolve(
					Generation,
					[]() {
						return FAggregateResult::Success({.Solid = 1, .Wire = 2});
					},
					Reporter),
				nullptr);
			Generation.Advance(EDependency::Shader);
			ASSERT_NE(
				Entry.Slot.Resolve(
					Generation,
					[]() {
						FPipelineAggregate Candidate{.Solid = 7};
						return FAggregateResult::Failure(MakeCacheFailure());
					},
					Reporter),
				nullptr);
			ASSERT_NE(Entry.Slot.GetPayload(), nullptr);
			EXPECT_EQ(Entry.Slot.GetPayload()->Solid, 1);
			EXPECT_EQ(Entry.Slot.GetPayload()->Wire, 2);
			EXPECT_EQ(
				Entry.Slot.GetAvailability(),
				ERenderResourceAvailability::StaleReady);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			DeviceInvalidationNeverReturnsOldPipelineAggregate)
		{
			FCache Cache(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto& Entry = Cache.FindOrAdd(4);
			ASSERT_NE(
				Entry.Slot.Resolve(
					Generation,
					[]() { return FResult::Success(44); },
					Reporter),
				nullptr);

			Generation.Advance(EDependency::Device);
			EXPECT_EQ(
				Entry.Slot.Resolve(
					Generation,
					[]() { return FResult::Failure(MakeCacheFailure()); },
					Reporter),
				nullptr);
			EXPECT_EQ(Entry.Slot.GetPayload(), nullptr);
		}
	}
}
