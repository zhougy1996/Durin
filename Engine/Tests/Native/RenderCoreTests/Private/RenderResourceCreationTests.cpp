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
	}
}
