#include <gtest/gtest.h>

#include "RendererResourceSlotCache.h"
#include "Renderers/MaterialResourceCacheKeyHash.h"
#include "Renderers/MeshRenderPreparationCommon.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	namespace
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<int>;
		using FCache = TRendererResourceSlotCache<int, int>;

		TEST(FRendererResourceSlotCacheTests, LookupAndRetentionBaseline)
		{
			if (std::getenv("DURIN_RENDERER_SLOT_BENCHMARK") == nullptr) GTEST_SKIP();
			const auto Root = Testing::CreateTestFixtureDirectory("RendererSlotBaseline");
			std::string Csv = "working_set,scenario,run,sample,nanoseconds,recreations,evictions,retained_payload_bytes,entries,accounted_slot_bytes\n";
			for (const int WorkingSet : {256, 512, 1024, 4096})
			for (const bool Hot : {false, true})
			for (int Run = 0; Run < 3; ++Run)
			{
				FCache Cache;
				FRenderResourceGeneration Generation;
				for (int Sample = -5; Sample < 30; ++Sample)
				{
					size_t Recreations = 0, Evictions = 0;
					const auto Start = std::chrono::steady_clock::now();
					for (int Access = 0; Access < WorkingSet; ++Access)
					{
						const int Key = Hot && Access % 10 != 0 ? Access % 64
							: Hot ? 64 + (Access / 10 + (Sample + 5) * ((WorkingSet + 9) / 10)) % (WorkingSet - 64) : Access;
						const size_t Before = Cache.Num();
						auto& Entry = Cache.FindOrAddBounded(Key, 512);
						const bool Missing = Entry.Slot.GetPayload() == nullptr;
						if (Missing && Before == 512) ++Evictions;
						Entry.Slot.Resolve(Generation, [&] {
							++Recreations;
							return FResult::Success(Key);
						}, [](const FRenderResourceCreateDiagnostic&) {});
					}
					const auto Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - Start).count();
					const auto SlotBytes = []<typename T>(const T& Value) -> size_t {
						if constexpr (requires { Value.GetAccountedSlotBytes(); }) return Value.GetAccountedSlotBytes();
						else return Value.Num() * sizeof(typename T::FEntry);
					}(Cache);
					if (Sample >= 0) Csv += std::format("{},{},{},{},{},{},{},{},{},{}\n", WorkingSet,
						Hot ? "hot" : "cyclic", Run, Sample, Ns, Recreations, Evictions,
						Cache.GetRetainedPayloadWeight([](int, int) { return sizeof(int); }), Cache.Num(), SlotBytes);
				}
			}
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Csv)), Root / "samples.csv"));
			std::cout << "[RendererSlotBaseline] csv=" << (Root / "samples.csv").generic_string() << '\n';
		}

		TEST(FRendererResourceSlotCacheTests, PreparedPayloadSurvivesGrowthEvictionAndReset)
		{
			using FPayload = std::shared_ptr<const int>;
			using FOwnedResult = TRenderResourceCreateResult<FPayload>;
			TRendererResourceSlotCache<int, FPayload> Cache(EDependency::Shader);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			const auto* Resolved = Cache.FindOrAdd(1).Slot.Resolve(Generation,
				[] { return FOwnedResult::Success(std::make_shared<const int>(42)); }, Reporter);
			ASSERT_NE(Resolved, nullptr);
			FPayload PreparedDraw = *Resolved;
			std::weak_ptr<const int> Lifetime = PreparedDraw;
			for (int Key = 2; Key < 128; ++Key) Cache.FindOrAdd(Key);
			EXPECT_EQ(*PreparedDraw, 42);
			EXPECT_EQ(*std::as_const(Cache).Find(1)->Slot.GetPayload(), PreparedDraw);
			Cache.FindOrAddBounded(128, 127);
			EXPECT_EQ(Cache.Find(1), nullptr);
			EXPECT_FALSE(Lifetime.expired());
			Cache.Reset();
			EXPECT_EQ(*PreparedDraw, 42);
			PreparedDraw.reset();
			EXPECT_TRUE(Lifetime.expired());
		}

		TEST(FRendererResourceSlotCacheTests, NewGenerationPublishesWithoutChangingPreparedPayload)
		{
			using FPayload = std::shared_ptr<const int>;
			using FOwnedResult = TRenderResourceCreateResult<FPayload>;
			TRendererResourceSlotCache<int, FPayload> Cache(EDependency::Shader);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			int Builds = 0;
			auto Build = [&] { return FOwnedResult::Success(std::make_shared<const int>(++Builds)); };
			auto& Slot = Cache.FindOrAdd(1).Slot;
			const auto* First = Slot.Resolve(Generation, Build, Reporter);
			ASSERT_NE(First, nullptr);
			FPayload PreparedDraw = *First;
			EXPECT_EQ(*Slot.Resolve(Generation, Build, Reporter), PreparedDraw);
			EXPECT_EQ(Builds, 1);
			Generation.Advance(EDependency::Shader);
			const auto* Second = Slot.Resolve(Generation, Build, Reporter);
			ASSERT_NE(Second, nullptr);
			EXPECT_NE(*Second, PreparedDraw);
			EXPECT_EQ(**Second, 2);
			EXPECT_EQ(*PreparedDraw, 1);
		}

		auto MakeCacheFailure() -> FRenderResourceCreateError
		{
			return {
				.Category =
					ERenderResourceCreateErrorCategory::GraphicsPipeline,
				.Context = "StaticMeshPipeline",
				.Identity = "test-key",
				.Cause = {},
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
				.Cause = std::string("Shader entry-point request is invalid."),
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
			FailedKeyRetriesOnNextPreparationWithoutPoisoningOtherKey)
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
			EXPECT_NE(
				FirstEntry.Slot.Resolve(
					Generation,
					[&]() {
						++FirstKeyAttempts;
						return FResult::Success(11);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(FirstKeyAttempts, 2);

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
			EXPECT_EQ(FirstKeyAttempts, 3);
			EXPECT_EQ(*Cache.Find(2)->Slot.GetPayload(), 22);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			LateAggregateFailureExposesNeitherOldNorPartialPayload)
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
			EXPECT_EQ(
				Entry.Slot.Resolve(
					Generation,
					[]() {
						FPipelineAggregate Candidate{.Solid = 7};
						return FAggregateResult::Failure(MakeCacheFailure());
					},
					Reporter),
				nullptr);
			EXPECT_EQ(Entry.Slot.GetPayload(), nullptr);
			EXPECT_EQ(
				Entry.Slot.GetAvailability(),
				ERenderResourceAvailability::Failed);
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

		TEST(
			FRendererResourceSlotCacheTests,
			FailedSceneTargetCandidateRecoversOnNextPreparation)
		{
			struct FSceneTargets
			{
				int Color = 0;
				int Depth = 0;
			};
			using FSceneTargetResult =
				TRenderResourceCreateResult<FSceneTargets>;
			TRendererResourceSlotCache<int, FSceneTargets> Cache(
				EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto& Entry = Cache.FindOrAdd(1920);

			EXPECT_EQ(
				Entry.Slot.Resolve(
					Generation,
					[&]() {
						++Attempts;
						return FSceneTargetResult::Failure(
							MakeCacheFailure());
					},
					[](const FRenderResourceCreateDiagnostic&) {}),
				nullptr);
			EXPECT_EQ(Entry.Slot.GetPayload(), nullptr);
			EXPECT_EQ(
				Entry.Slot.GetAvailability(),
				ERenderResourceAvailability::Failed);

			EXPECT_NE(
				Entry.Slot.Resolve(
					Generation,
					[&]() {
						++Attempts;
						return FSceneTargetResult::Success({1, 2});
					},
					[](const FRenderResourceCreateDiagnostic&) {}),
				nullptr);
			EXPECT_EQ(Attempts, 2);

			Generation.Advance(EDependency::Manual);
			FSceneTargets* Recovered = Entry.Slot.Resolve(
				Generation,
				[&]() {
					++Attempts;
					return FSceneTargetResult::Success({1, 2});
				},
				[](const FRenderResourceCreateDiagnostic&) {});
			ASSERT_NE(Recovered, nullptr);
			EXPECT_EQ(Recovered->Color, 1);
			EXPECT_EQ(Recovered->Depth, 2);
			EXPECT_EQ(Attempts, 2);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			BoundedCacheEvictsOldestIdentityWithoutEvictingCurrent)
		{
			FCache Cache;
			Cache.FindOrAdd(1);
			Cache.FindOrAdd(2);
			Cache.FindOrAdd(3);

			EXPECT_TRUE(Cache.EvictOldestExcept(3));
			EXPECT_EQ(Cache.Find(1), nullptr);
			EXPECT_NE(Cache.Find(2), nullptr);
			EXPECT_NE(Cache.Find(3), nullptr);
			EXPECT_EQ(Cache.Num(), 2);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			FindOrAddBoundedPromotesExistingAndEvictsLeastRecentlyUsed)
		{
			FCache Cache;
			Cache.FindOrAddBounded(1, 2);
			Cache.FindOrAddBounded(2, 2);
			EXPECT_EQ(Cache.FindOrAddBounded(1, 2).Key, 1);
			EXPECT_EQ(Cache.Num(), 2u);
			Cache.FindOrAddBounded(3, 2);
			EXPECT_EQ(Cache.Num(), 2u);
			EXPECT_NE(Cache.Find(1), nullptr);
			EXPECT_EQ(Cache.Find(2), nullptr);
			EXPECT_NE(Cache.Find(3), nullptr);
		}

		TEST(FRendererResourceSlotCacheTests, CollisionsPreserveExactKeysAndStableSlots)
		{
			struct FConstantHash { auto operator()(int) const -> size_t { return 0; } };
			TRendererResourceSlotCache<int, int, FConstantHash> Cache;
			auto* First = &Cache.FindOrAdd(1);
			for (int Key = 2; Key < 128; ++Key) Cache.FindOrAdd(Key);
			EXPECT_EQ(Cache.Find(1), First);
			EXPECT_EQ(Cache.GetHits(), 1u);
			EXPECT_EQ(Cache.GetMisses(), 127u);
			for (int Key = 2; Key < 128; ++Key) EXPECT_EQ(Cache.Find(Key)->Key, Key);
			Cache.FindOrAddBounded(128, 127);
			EXPECT_EQ(std::as_const(Cache).Find(1), nullptr);
			EXPECT_EQ(Cache.GetEvictions(), 1u);
			Cache.Reset();
			EXPECT_EQ(Cache.Num(), 0u);
			EXPECT_EQ(Cache.GetHits(), 0u);
			EXPECT_EQ(Cache.FindOrAdd(1).Index, 0u);
		}

		TEST(FRendererResourceSlotCacheTests, LoweredCountAndByteBudgetsApplyOnHitsAndMisses)
		{
			FCache Cache;
			for (int Key = 0; Key < 1024; ++Key) Cache.FindOrAdd(Key);
			const size_t Budget = FCache::AccountedEntryBytes * 4 + 128;
			EXPECT_EQ(Cache.FindOrAddBounded(0, 8, Budget).Key, 0);
			EXPECT_LE(Cache.Num(), 4u);
			EXPECT_LE(Cache.GetAccountedSlotBytes(), Budget);
			EXPECT_NE(std::as_const(Cache).Find(0), nullptr);
			EXPECT_EQ(Cache.FindOrAddBounded(1, 1, FCache::MinimumByteBudget).Key, 1);
			EXPECT_EQ(Cache.Num(), 1u);
			EXPECT_LE(Cache.GetAccountedSlotBytes(), FCache::MinimumByteBudget);
			EXPECT_EQ(std::as_const(Cache).Find(0), nullptr);
		}

		TEST(FRendererResourceSlotCacheTests, HotWorkingSetSurvivesColdAdmissions)
		{
			FCache Cache;
			for (int Key = 0; Key < 8; ++Key) Cache.FindOrAddBounded(Key, 16);
			for (int Cold = 8; Cold < 4096; ++Cold)
			{
				for (int Hot = 0; Hot < 8; ++Hot) ASSERT_NE(Cache.Find(Hot), nullptr);
				Cache.FindOrAddBounded(Cold, 16);
			}
			EXPECT_EQ(Cache.Num(), 16u);
			EXPECT_EQ(Cache.GetEvictions(), 4080u);
		}

		TEST(FRendererResourceSlotCacheTests, MaterialHashUsesSemanticValuesAndExactPipelineEquality)
		{
			using FHash = RendererPrivate::FMaterialResourceCacheKeyHash;
			TRendererResourceSlotCache<FEffectiveMeshPipelineKey, int, FHash> Cache;
			FEffectiveMeshPipelineKey First;
			First.Material.ShaderMap.OpacityMaskThreshold = 0.0f;
			auto Equivalent = First;
			Equivalent.Material.ShaderMap.OpacityMaskThreshold = -0.0f;
			EXPECT_EQ(FHash{}(First), FHash{}(Equivalent));
			EXPECT_EQ(&Cache.FindOrAdd(First), &Cache.FindOrAdd(Equivalent));
			auto Different = First;
			++Different.Depth.StencilReference; // Not hashed, but still part of equality.
			EXPECT_EQ(FHash{}(First), FHash{}(Different));
			EXPECT_NE(&Cache.FindOrAdd(First), &Cache.FindOrAdd(Different));
			EXPECT_EQ(Cache.Num(), 2u);
		}

		TEST(
			FRendererResourceSlotCacheTests,
			RetainedPayloadWeightIgnoresFailedSlotsAndTracksEviction)
		{
			FCache Cache;
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			ASSERT_NE(Cache.FindOrAdd(2).Slot.Resolve(
				Generation,
				[] { return FResult::Success(20); },
				Reporter), nullptr);
			ASSERT_NE(Cache.FindOrAdd(3).Slot.Resolve(
				Generation,
				[] { return FResult::Success(30); },
				Reporter), nullptr);
			Cache.FindOrAdd(4);
			auto Weight = [&Cache] {
				return Cache.GetRetainedPayloadWeight(
					[](int Key, int Payload) {
						return static_cast<uint64>(Key + Payload);
					});
			};
			EXPECT_EQ(Weight(), 55u);
			EXPECT_EQ(
				Cache.GetRetainedPayloadWeight(
					[](int, int) { return std::numeric_limits<uint64>::max(); }),
				std::numeric_limits<uint64>::max());
			EXPECT_TRUE(Cache.EvictOldestExcept(3));
			EXPECT_EQ(Weight(), 33u);
		}
	}
}
