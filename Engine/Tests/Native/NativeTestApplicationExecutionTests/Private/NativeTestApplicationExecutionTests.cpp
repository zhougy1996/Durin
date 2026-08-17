#include <gtest/gtest.h>

#include <cstdlib>
#include <string_view>

TEST(NativeTestApplicationExecution, ReceivesControllerEnvironment)
{
	const char* Path = std::getenv("PATH");
	ASSERT_NE(Path, nullptr);
	EXPECT_FALSE(std::string_view(Path).empty());
}

TEST(NativeTestApplicationExecution, PublishesReportableSuccess)
{
	SUCCEED();
}
