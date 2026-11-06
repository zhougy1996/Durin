#include "MonaImGui.h"
#include "Viewport/ViewportClosePolicy.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	class FViewportClosePolicyTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override { Testing::InitializeDObjectSystemForTests(); }
	};

	TEST_F(FViewportClosePolicyTests, PendingOwnerVetoesNativeCloseUntilItsOperationCompletes)
	{
		bool Pending = true;
		int Requests = 0;
		auto Registration = MonaImGui::RegisterViewportClosePolicy(3141, [&] { ++Requests; return !Pending; });
		ASSERT_TRUE(Registration);
		EXPECT_FALSE(MonaImGui::TryCloseViewport(3141));
		Pending = false;
		EXPECT_TRUE(MonaImGui::TryCloseViewport(3141));
		EXPECT_EQ(2, Requests);
	}

	TEST_F(FViewportClosePolicyTests, DuplicatePolicyCannotReplaceTheOriginalOwnersVeto)
	{
		auto Original = MonaImGui::RegisterViewportClosePolicy(3141, [] { return false; });
		auto Duplicate = MonaImGui::RegisterViewportClosePolicy(3141, [] { return true; });
		EXPECT_TRUE(Original);
		EXPECT_FALSE(Duplicate);
		EXPECT_FALSE(MonaImGui::TryCloseViewport(3141));
	}

	TEST_F(FViewportClosePolicyTests, OwnerCanUnregisterDuringCloseWithoutInvalidatingItsCallback)
	{
		MonaImGui::FViewportClosePolicyRegistration Registration;
		Registration = MonaImGui::RegisterViewportClosePolicy(3141, [&] {
			Registration.Reset();
			return false;
		});
		EXPECT_FALSE(MonaImGui::TryCloseViewport(3141));
		EXPECT_TRUE(MonaImGui::TryCloseViewport(3141));
	}

	TEST_F(FViewportClosePolicyTests, OldTokenCannotRemoveAPolicyRegisteredAfterBackendShutdown)
	{
		auto Original = MonaImGui::RegisterViewportClosePolicy(3141, [] { return true; });
		MonaImGui::Shutdown();
		auto Replacement = MonaImGui::RegisterViewportClosePolicy(3141, [] { return false; });
		ASSERT_TRUE(Replacement);
		Original.Reset();
		EXPECT_FALSE(MonaImGui::TryCloseViewport(3141));
	}

	TEST_F(FViewportClosePolicyTests, UnownedViewportRetainsDefaultCloseBehavior)
	{
		EXPECT_TRUE(MonaImGui::TryCloseViewport(3141));
		EXPECT_FALSE(MonaImGui::RegisterViewportClosePolicy(0, [] { return true; }));
		EXPECT_FALSE(MonaImGui::RegisterViewportClosePolicy(3141, {}));
		EXPECT_EQ(nullptr, MonaImGui::GetWindowForViewport(3141));
	}
}
