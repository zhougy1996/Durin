#include <gtest/gtest.h>

#include "RenderResourceCreation.h"
#include "RendererResourceInvalidation.h"

namespace Durin
{
	TEST(FRendererResourceInvalidationTests,
		CommandsQueueExactCausesAndShutdownRejectsCopiedCallbacks)
	{
		FConsoleCommandRegistry Registry;
		FRendererResourceInvalidationController Controller;
		std::vector<ERendererResourceInvalidationCause> QueuedCauses;
		ASSERT_TRUE(Controller.Start(
			Registry,
			[&QueuedCauses](ERendererResourceInvalidationCause Cause) {
				QueuedCauses.push_back(Cause);
			}));

		const FConsoleCommandResult Changed =
			Registry.Execute("renderer.reload-shaders changed");
		const FConsoleCommandResult All =
			Registry.Execute("renderer.reload-shaders all");
		const FConsoleCommandResult Retry =
			Registry.Execute("renderer.retry-resources");
		EXPECT_TRUE(Changed.bSuccess);
		EXPECT_TRUE(All.bSuccess);
		EXPECT_TRUE(Retry.bSuccess);
		EXPECT_NE(Changed.Message.find("lazily"), std::string::npos);
		EXPECT_NE(All.Message.find("forced"), std::string::npos);
		EXPECT_NE(Retry.Message.find("demand-driven"), std::string::npos);
		EXPECT_FALSE(
			Registry.Execute("renderer.reload-shaders unexpected").bSuccess);

		const std::array Expected{
			ERendererResourceInvalidationCause::ShaderChanged,
			ERendererResourceInvalidationCause::ShaderAll,
			ERendererResourceInvalidationCause::ManualRetry,
		};
		EXPECT_TRUE(std::ranges::equal(QueuedCauses, Expected));

		const std::vector<FConsoleCommandDesc> Registered =
			Registry.GetCommands();
		const auto ReloadIt = std::ranges::find(
			Registered,
			std::string("renderer.reload-shaders"),
			&FConsoleCommandDesc::Name);
		ASSERT_NE(ReloadIt, Registered.end());
		const FConsoleCommandDesc CopiedReload = *ReloadIt;
		Controller.Stop();

		EXPECT_FALSE(
			Registry.Execute("renderer.reload-shaders changed").bSuccess);
		const std::array<std::string, 1> ChangedArgs{"changed"};
		EXPECT_FALSE(CopiedReload.Execute(ChangedArgs).bSuccess);
		EXPECT_EQ(QueuedCauses.size(), Expected.size());
	}

	TEST(FRendererResourceInvalidationTests,
		QueuedDeviceInvalidationDropsFallbackAndManualRetryRecovers)
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FPayload = std::shared_ptr<int>;
		using FResult = TRenderResourceCreateResult<FPayload>;

		FConsoleCommandRegistry Registry;
		FRendererResourceInvalidationController Controller;
		std::vector<ERendererResourceInvalidationCause> QueuedCauses;
		ASSERT_TRUE(Controller.Start(
			Registry,
			[&QueuedCauses](ERendererResourceInvalidationCause Cause) {
				QueuedCauses.push_back(Cause);
			}));

		FRenderResourceGeneration Generation;
		TRenderResourceCreationSlot<FPayload> Slot{
			EDependency::Shader | EDependency::Device};
		const auto IgnoreDiagnostic =
			[](const FRenderResourceCreateDiagnostic&) {};
		auto* Initial = Slot.Resolve(
			Generation,
			[] { return FResult::Success(std::make_shared<int>(7)); },
			IgnoreDiagnostic);
		ASSERT_NE(Initial, nullptr);
		std::weak_ptr<int> OldPayload = *Initial;
		int Attempts = 1;

		ASSERT_TRUE(Controller.Request(
			ERendererResourceInvalidationCause::Device).bSuccess);
		EXPECT_NE(
			Slot.Resolve(Generation, [&Attempts] {
				++Attempts;
				return FResult::Success(std::make_shared<int>(8));
			}, IgnoreDiagnostic),
			nullptr);
		EXPECT_EQ(Attempts, 1);
		ASSERT_EQ(QueuedCauses.size(), 1);

		Generation.Advance(EDependency::Device);
		QueuedCauses.erase(QueuedCauses.begin());
		EXPECT_EQ(
			Slot.Resolve(Generation, [&Attempts]() -> FResult {
				++Attempts;
				return FResult::Failure({
					.Category =
						ERenderResourceCreateErrorCategory::RHIResource,
					.Context = "DeviceInvalidationTest",
					.Identity = "payload",
					.Message = "injected RHI failure",
					.RetryDependencies = EDependency::Manual,
				});
			},
			IgnoreDiagnostic),
			nullptr);
		EXPECT_TRUE(OldPayload.expired());
		EXPECT_EQ(
			Slot.GetAvailability(),
			ERenderResourceAvailability::Failed);

		ASSERT_TRUE(
			Registry.Execute("renderer.retry-resources").bSuccess);
		ASSERT_EQ(QueuedCauses.size(), 1);
		Generation.Advance(EDependency::Manual);
		QueuedCauses.clear();
		auto* Recovered = Slot.Resolve(
			Generation,
			[&Attempts] {
				++Attempts;
				return FResult::Success(std::make_shared<int>(9));
			},
			IgnoreDiagnostic);
		ASSERT_NE(Recovered, nullptr);
		EXPECT_EQ(**Recovered, 9);
		EXPECT_EQ(Attempts, 3);
	}

	TEST(FRendererResourceInvalidationTests,
		ChangedRetriesCorrectedShaderAndAllMarksForcedGeneration)
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<std::string>;

		FConsoleCommandRegistry Registry;
		FRendererResourceInvalidationController Controller;
		std::vector<ERendererResourceInvalidationCause> QueuedCauses;
		ASSERT_TRUE(Controller.Start(
			Registry,
			[&QueuedCauses](ERendererResourceInvalidationCause Cause) {
				QueuedCauses.push_back(Cause);
			}));

		FRenderResourceGeneration Generation;
		std::optional<uint64> ForceRecompileGeneration;
		TRenderResourceCreationSlot<std::string> Slot{
			EDependency::Shader};
		std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
		bool bSourceValid = false;
		int Attempts = 0;
		auto Resolve = [&]() {
			return Slot.Resolve(
				Generation,
				[&]() -> FResult {
					++Attempts;
					if (!bSourceValid)
					{
						return FResult::Failure({
							.Category =
								ERenderResourceCreateErrorCategory::
									ShaderCompile,
							.Context = "ShaderReloadTest",
							.Identity = "correctable",
							.Message = "injected shader compile failure",
							.RetryDependencies = EDependency::Shader,
						});
					}
					const bool bForced =
						ForceRecompileGeneration == Generation.Shader;
					return FResult::Success(
						bForced ? "forced" : "changed");
				},
				[&Diagnostics](
					FRenderResourceCreateDiagnostic Diagnostic) {
					Diagnostics.push_back(std::move(Diagnostic));
				});
		};

		EXPECT_EQ(Resolve(), nullptr);
		EXPECT_EQ(Resolve(), nullptr);
		EXPECT_EQ(Attempts, 1);
		bSourceValid = true;
		ASSERT_TRUE(
			Registry.Execute("renderer.reload-shaders changed").bSuccess);
		EXPECT_EQ(Resolve(), nullptr);
		EXPECT_EQ(Attempts, 1);
		ASSERT_EQ(QueuedCauses.size(), 1);
		ASSERT_EQ(
			QueuedCauses.front(),
			ERendererResourceInvalidationCause::ShaderChanged);
		Generation.Advance(EDependency::Shader);
		ForceRecompileGeneration.reset();
		QueuedCauses.clear();
		auto* Corrected = Resolve();
		ASSERT_NE(Corrected, nullptr);
		EXPECT_EQ(*Corrected, "changed");
		EXPECT_EQ(Attempts, 2);
		ASSERT_EQ(Diagnostics.size(), 2);
		EXPECT_EQ(
			Diagnostics.back().Kind,
			ERenderResourceCreateDiagnosticKind::Recovery);

		ASSERT_TRUE(
			Registry.Execute("renderer.reload-shaders all").bSuccess);
		ASSERT_EQ(QueuedCauses.size(), 1);
		ASSERT_EQ(
			QueuedCauses.front(),
			ERendererResourceInvalidationCause::ShaderAll);
		Generation.Advance(EDependency::Shader);
		ForceRecompileGeneration = Generation.Shader;
		QueuedCauses.clear();
		auto* Forced = Resolve();
		ASSERT_NE(Forced, nullptr);
		EXPECT_EQ(*Forced, "forced");
		EXPECT_EQ(Attempts, 3);
	}
} // namespace Durin
