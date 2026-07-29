#include <gtest/gtest.h>

#include "RenderResourceCreation.h"

namespace Durin
{
	namespace
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<int>;
		using FSlot = TRenderResourceCreationSlot<int>;

		auto MakeError(
			EDependency RetryDependencies = EDependency::Shader)
			-> FRenderResourceCreateError
		{
			return {
				.Category = ERenderResourceCreateErrorCategory::ShaderCompile,
				.Context = "TestResource",
				.Identity = "variant=1",
				.Message = "injected compile failure",
				.RetryDependencies = RetryDependencies,
			};
		}

		TEST(
			FRenderResourceCreationTests,
			InitialFailureIsSuppressedUntilRelevantGenerationChanges)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Failure(MakeError())
					: FResult::Success(42);
			};
			auto Reporter = [&](FRenderResourceCreateDiagnostic Diagnostic) {
				Diagnostics.push_back(std::move(Diagnostic));
			};

			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);
			EXPECT_EQ(Diagnostics.size(), 1);

			++Generation.Manual;
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);

			++Generation.Shader;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 42);
			EXPECT_EQ(Attempts, 2);
			ASSERT_EQ(Diagnostics.size(), 2);
			EXPECT_EQ(
				Diagnostics.back().Kind,
				ERenderResourceCreateDiagnosticKind::Recovery);
			ASSERT_TRUE(Diagnostics.back().Error.has_value());
			EXPECT_EQ(Diagnostics.back().Error->Context, "TestResource");
			EXPECT_EQ(Diagnostics.back().Error->Identity, "variant=1");
		}

		TEST(
			FRenderResourceCreationTests,
			LateFailureDoesNotPublishPartialCandidate)
		{
			struct FCandidate
			{
				int FirstStep = 0;
				int SecondStep = 0;
			};
			using FCandidateResult = TRenderResourceCreateResult<FCandidate>;
			TRenderResourceCreationSlot<FCandidate> Slot(EDependency::Device);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};

			FCandidate Candidate{.FirstStep = 7};
			EXPECT_EQ(
				Slot.Resolve(
					Generation,
					[&]() {
						return FCandidateResult::Failure(
							MakeError(EDependency::Device));
					},
					Reporter),
				nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
		}

		TEST(
			FRenderResourceCreationTests,
			FailedShaderRefreshRetainsLastKnownGoodPayload)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				if (Attempts == 2)
				{
					EXPECT_EQ(
						Slot.GetAvailability(),
						ERenderResourceAvailability::Refreshing);
					EXPECT_EQ(*Slot.GetPayload(), 11);
				}
				return Attempts == 1
					? FResult::Success(11)
					: FResult::Failure(MakeError());
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			++Generation.Shader;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 11);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::StaleReady);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			EXPECT_TRUE(Slot.GetFailure()->bRetainedFallback);
			EXPECT_EQ(Attempts, 2);
		}

		TEST(
			FRenderResourceCreationTests,
			SuccessfulRefreshAtomicallyReplacesPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				if (Attempts == 2)
				{
					EXPECT_EQ(
						Slot.GetAvailability(),
						ERenderResourceAvailability::Refreshing);
					EXPECT_EQ(*Slot.GetPayload(), 4);
				}
				return FResult::Success(Attempts == 1 ? 4 : 8);
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 4);
			Generation.Advance(EDependency::Shader);
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(*Slot.GetPayload(), 8);
			EXPECT_EQ(
				Slot.GetPayloadGeneration().Shader,
				Generation.Shader);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Ready);
		}

		TEST(
			FRenderResourceCreationTests,
			DeviceGenerationClearsFallbackBeforeRetry)
		{
			FSlot Slot(EDependency::Shader | EDependency::Device);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Success(9)
					: FResult::Failure(MakeError(EDependency::Device));
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			++Generation.Device;
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Failed);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			EXPECT_FALSE(Slot.GetFailure()->bRetainedFallback);
		}

		TEST(
			FRenderResourceCreationTests,
			ReentrantResolveDoesNotInvokeFactoryTwiceOrExposePartialPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			auto Factory = [&]() -> FResult {
				++Attempts;
				EXPECT_EQ(
					Slot.GetAvailability(),
					ERenderResourceAvailability::Creating);
				EXPECT_EQ(Slot.GetPayload(), nullptr);
				EXPECT_EQ(
					Slot.Resolve(
						Generation,
						[]() { return FResult::Success(99); },
						Reporter),
					nullptr);
				return FResult::Success(5);
			};

			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Attempts, 1);
			EXPECT_EQ(*Slot.GetPayload(), 5);
		}

		TEST(
			FRenderResourceCreationTests,
			ReentrantRefreshReturnsOnlyLastKnownGoodPayload)
		{
			FSlot Slot(EDependency::Shader);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			ASSERT_NE(
				Slot.Resolve(
					Generation,
					[]() { return FResult::Success(6); },
					Reporter),
				nullptr);
			Generation.Advance(EDependency::Shader);
			int NestedFactoryCalls = 0;

			ASSERT_NE(
				Slot.Resolve(
					Generation,
					[&]() {
						EXPECT_EQ(
							Slot.GetAvailability(),
							ERenderResourceAvailability::Refreshing);
						int* Fallback = Slot.Resolve(
							Generation,
							[&]() {
								++NestedFactoryCalls;
								return FResult::Success(99);
							},
							Reporter);
						EXPECT_NE(Fallback, nullptr);
						if (Fallback != nullptr)
						{
							EXPECT_EQ(*Fallback, 6);
						}
						return FResult::Success(7);
					},
					Reporter),
				nullptr);
			EXPECT_EQ(NestedFactoryCalls, 0);
			EXPECT_EQ(*Slot.GetPayload(), 7);
		}

		TEST(
			FRenderResourceCreationTests,
			RepeatedFailureReportsOnceAndRecoveryReportsOnce)
		{
			FSlot Slot(EDependency::Manual);
			FRenderResourceGeneration Generation;
			int Attempts = 0;
			std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
			auto Factory = [&]() {
				++Attempts;
				return Attempts == 1
					? FResult::Failure(MakeError(EDependency::Manual))
					: FResult::Success(3);
			};
			auto Reporter = [&](FRenderResourceCreateDiagnostic Diagnostic) {
				Diagnostics.push_back(std::move(Diagnostic));
			};

			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			EXPECT_EQ(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			ASSERT_EQ(Diagnostics.size(), 1);
			++Generation.Manual;
			ASSERT_NE(Slot.Resolve(Generation, Factory, Reporter), nullptr);
			ASSERT_EQ(Diagnostics.size(), 2);
			EXPECT_EQ(
				Diagnostics.back().Kind,
				ERenderResourceCreateDiagnosticKind::Recovery);
		}

		TEST(
			FRenderResourceCreationTests,
			FailureFingerprintTracksOwnedDiagnosticAndResetClearsState)
		{
			FSlot Slot(EDependency::Manual);
			FRenderResourceGeneration Generation;
			auto Reporter = [](const FRenderResourceCreateDiagnostic&) {};
			const FRenderResourceCreateError Error =
				MakeError(EDependency::Manual);

			EXPECT_EQ(
				Slot.Resolve(
					Generation,
					[&]() { return FResult::Failure(Error); },
					Reporter),
				nullptr);
			ASSERT_NE(Slot.GetFailure(), nullptr);
			ASSERT_TRUE(Slot.GetFailureFingerprint().has_value());
			EXPECT_EQ(
				*Slot.GetFailureFingerprint(),
				Slot.GetFailure()->GetFingerprint());
			EXPECT_EQ(
				Slot.GetAttemptedGeneration(),
				Generation);

			Slot.Reset();
			EXPECT_EQ(Slot.GetPayload(), nullptr);
			EXPECT_EQ(Slot.GetFailure(), nullptr);
			EXPECT_FALSE(Slot.GetFailureFingerprint().has_value());
			EXPECT_EQ(
				Slot.GetAvailability(),
				ERenderResourceAvailability::Uninitialized);
		}
	}
}
