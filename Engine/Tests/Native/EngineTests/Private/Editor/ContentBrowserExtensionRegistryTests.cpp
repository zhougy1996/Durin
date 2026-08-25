#include <gtest/gtest.h>

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Modules/ModuleTestSupport.h"

namespace Durin::Editor::ContentBrowser
{
	TEST(FContentBrowserExtensionRegistryTests, OrdersByOrderThenStableId)
	{
		FModuleTestOwner Owner("ContentBrowserExtensionRegistryTests.Order");
		auto Gate = Owner.CreateOwnedCallbackRegistration("ContentBrowser.Extensions");
		std::string Error;
		auto B = RegisterExtension({
			.Id = "test.order.b", .Label = "B",
			.Category = EExtensionCategory::Create, .Order = 20,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}, .OwnerGate = Gate.GetGate()}, Error);
		auto A = RegisterExtension({
			.Id = "test.order.a", .Label = "A",
			.Category = EExtensionCategory::Create, .Order = 20,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}, .OwnerGate = Gate.GetGate()}, Error);
		auto First = RegisterExtension({
			.Id = "test.order.first", .Label = "First",
			.Category = EExtensionCategory::Create, .Order = 10,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}, .OwnerGate = Gate.GetGate()}, Error);
		ASSERT_TRUE(A.IsValid());
		ASSERT_TRUE(B.IsValid());
		ASSERT_TRUE(First.IsValid());

		const auto Snapshot = CaptureExtensions(EExtensionCategory::Create);
		const auto FindIndex = [&Snapshot](std::string_view Id) {
			return std::distance(Snapshot.begin(), std::ranges::find(
				Snapshot, Id, &FExtensionDescriptor::Id));
		};
		EXPECT_LT(FindIndex("test.order.first"), FindIndex("test.order.a"));
		EXPECT_LT(FindIndex("test.order.a"), FindIndex("test.order.b"));
	}

	TEST(FContentBrowserExtensionRegistryTests, RejectsDuplicateAndRetiredOwnerInvocation)
	{
		FModuleTestOwner Owner("ContentBrowserExtensionRegistryTests.Lifetime");
		auto Gate = Owner.CreateOwnedCallbackRegistration("ContentBrowser.Extensions");
		int Invocations = 0;
		std::string Error;
		auto Registration = RegisterExtension({
			.Id = "test.lifetime", .Label = "Lifetime",
			.Category = EExtensionCategory::Import,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [&Invocations](const auto&) { ++Invocations; },
			.OwnerGate = Gate.GetGate()}, Error);
		ASSERT_TRUE(Registration.IsValid());
		auto Duplicate = RegisterExtension({
			.Id = "test.lifetime", .Label = "Duplicate",
			.Category = EExtensionCategory::Import,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [](const auto&) {}, .OwnerGate = Gate.GetGate()}, Error);
		EXPECT_FALSE(Duplicate.IsValid());
		EXPECT_FALSE(Error.empty());

		const auto Snapshot = CaptureExtensions(EExtensionCategory::Import);
		const auto Entry = std::ranges::find(
			Snapshot, "test.lifetime", &FExtensionDescriptor::Id);
		ASSERT_NE(Entry, Snapshot.end());
		EXPECT_TRUE(InvokeExtension(*Entry,
			{.Context = {.VirtualDirectory = "/Project"}}));
		EXPECT_EQ(Invocations, 1);
		EXPECT_TRUE(Owner.BeginRetirement(std::chrono::milliseconds(0)).Succeeded());
		EXPECT_FALSE(InvokeExtension(*Entry,
			{.Context = {.VirtualDirectory = "/Project"}}));
		EXPECT_EQ(Invocations, 1);
		Registration.Reset();
	}
} // namespace Durin::Editor::ContentBrowser
