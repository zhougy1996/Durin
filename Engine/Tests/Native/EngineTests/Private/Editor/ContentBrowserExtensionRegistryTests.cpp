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

	TEST(FContentBrowserExtensionRegistryTests, RegistersInvokesAndRemovesEveryCategory)
	{
		FModuleTestOwner Owner("ContentBrowserExtensionRegistryTests.Categories");
		auto Gate = Owner.CreateOwnedCallbackRegistration("ContentBrowser.Extensions");
		const std::array Categories{
			EExtensionCategory::Create,
			EExtensionCategory::Import,
			EExtensionCategory::Reimport,
			EExtensionCategory::Details,
			EExtensionCategory::ContextMenu};
		std::vector<FScopedExtensionRegistration> Registrations;
		int Invocations = 0;
		for (size_t Index = 0; Index < Categories.size(); ++Index)
		{
			std::string Error;
			const std::string Id = std::format("test.category.{}", Index);
			auto Registration = RegisterExtension({
				.Id = Id,
				.Label = "Category",
				.Category = Categories[Index],
				.IsApplicable = [](const auto&) { return true; },
				.Invoke = [&Invocations](const auto&) { ++Invocations; },
				.OwnerGate = Gate.GetGate()}, Error);
			ASSERT_TRUE(Registration.IsValid()) << Error;
			const auto Snapshot = CaptureExtensions(Categories[Index]);
			const auto Entry = std::ranges::find(
				Snapshot, Id, &FExtensionDescriptor::Id);
			ASSERT_NE(Entry, Snapshot.end());
			EXPECT_TRUE(InvokeExtension(*Entry, {}));
			Registrations.push_back(std::move(Registration));
		}
		EXPECT_EQ(Invocations, static_cast<int>(Categories.size()));

		for (size_t Index = Registrations.size(); Index > 0; --Index)
			Registrations[Index - 1].Reset();
		for (size_t Index = 0; Index < Categories.size(); ++Index)
		{
			const std::string Id = std::format("test.category.{}", Index);
			const auto Snapshot = CaptureExtensions(Categories[Index]);
			EXPECT_EQ(std::ranges::find(Snapshot, Id, &FExtensionDescriptor::Id),
				Snapshot.end());
		}
	}

	TEST(FContentBrowserExtensionRegistryTests,
		RejectsMutatingInvocationsAndGatesHostPresenters)
	{
		FModuleTestOwner Owner("ContentBrowserExtensionRegistryTests.MutationPolicy");
		auto Gate = Owner.CreateOwnedCallbackRegistration("ContentBrowser.Extensions");
		int Invocations = 0;
		int Presentations = 0;
		bool bLastPresentationAllowedMutation = true;
		std::string Error;
		auto Registration = RegisterExtension({
			.Id = "test.mutation-policy",
			.Label = "Mutation Policy",
			.Category = EExtensionCategory::Import,
			.IsApplicable = [](const auto&) { return true; },
			.Invoke = [&Invocations](const auto&) { ++Invocations; },
			.DrawHostPresentation = [
				&Presentations,
				&bLastPresentationAllowedMutation](bool bAllowAssetMutation) {
				++Presentations;
				bLastPresentationAllowedMutation = bAllowAssetMutation;
			},
			.OwnerGate = Gate.GetGate()}, Error);
		ASSERT_TRUE(Registration.IsValid()) << Error;

		const auto Extensions = CaptureExtensions(EExtensionCategory::Import);
		const auto Entry = std::ranges::find(
			Extensions, "test.mutation-policy", &FExtensionDescriptor::Id);
		ASSERT_NE(Entry, Extensions.end());
		EXPECT_FALSE(InvokeExtension(*Entry, {.bAllowAssetMutation = false}));
		EXPECT_EQ(Invocations, 0);
		EXPECT_TRUE(InvokeExtension(*Entry, {.bAllowAssetMutation = true}));
		EXPECT_EQ(Invocations, 1);

		const auto Presenters = CaptureHostPresenters();
		const auto Presenter = std::ranges::find(
			Presenters, "test.mutation-policy", &FExtensionDescriptor::Id);
		ASSERT_NE(Presenter, Presenters.end());
		EXPECT_TRUE(DrawHostPresentation(*Presenter, false));
		EXPECT_EQ(Presentations, 1);
		EXPECT_FALSE(bLastPresentationAllowedMutation);

		EXPECT_TRUE(Owner.BeginRetirement(std::chrono::milliseconds(0)).Succeeded());
		EXPECT_FALSE(DrawHostPresentation(*Presenter, true));
		EXPECT_EQ(Presentations, 1);
	}
} // namespace Durin::Editor::ContentBrowser
