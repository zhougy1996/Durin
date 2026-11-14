#include "Misc/UnifiedError.h"
#include <gtest/gtest.h>

namespace
{
	constexpr Durin::UnifiedError::FErrorId Missing{"Files", "Missing"};
}

TEST(FUnifiedErrorTests, IdentitySurvivesPresentationChangesAndContextPropagation)
{
	using Durin::UnifiedError::FError;
	FError Original(Missing, "First wording");
	auto Propagated = Original;
	Propagated.PushContext("package", "/Game/Owner");
	EXPECT_EQ(Original, Propagated);
	EXPECT_EQ(Original, FError(Missing, "Localized or revised wording"));
	EXPECT_TRUE(Original.GetContexts().empty());
	ASSERT_EQ(Propagated.GetContexts().size(), 1u);
	EXPECT_EQ(Propagated.GetContexts()[0].Key, "package");
	EXPECT_EQ(Propagated.GetContexts()[0].Value, "/Game/Owner");
	EXPECT_NE(Original, FError({"Network", "Missing"}));
}

TEST(FUnifiedErrorTests, OwnsIdentityMessageAndContextsPastProducerLifetime)
{
	auto Produce = [] {
		std::string Domain = "Files", Code = "Missing", Message = "Unavailable";
		Durin::UnifiedError::FError Error({Domain, Code}, Message);
		Error.PushContext("path", "source.bin");
		return Error;
	};
	const auto Error = Produce();
	EXPECT_TRUE(Error.Is(Missing));
	EXPECT_EQ(Error.GetMessage(), "Unavailable");
	ASSERT_EQ(Error.GetContexts().size(), 1u);
	EXPECT_EQ(Error.GetContexts()[0].Value, "source.bin");
}

TEST(FUnifiedErrorTests, EmptyMessageIsStillAnErrorWithPrintableIdentity)
{
	std::optional<Durin::UnifiedError::FError> Absent;
	std::optional<Durin::UnifiedError::FError> Present{std::in_place, Missing};
	EXPECT_FALSE(Absent.has_value());
	ASSERT_TRUE(Present.has_value());
	EXPECT_TRUE(Present->GetMessage().empty());
	EXPECT_TRUE(Present->Is(Missing));
	EXPECT_FALSE(Present->ToString().empty());
	EXPECT_EQ(std::format("{}", *Present), Present->ToString());
}
