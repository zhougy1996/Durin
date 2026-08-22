#include "CoreMinimal.h"
#include "Templates/CheckedArithmetic.h"

#include <gtest/gtest.h>

TEST(FCheckedArithmeticTests, EnforcesBoundsWithoutChangingRejectedOutputs)
{
	Durin::uint64 Value = 91;
	EXPECT_TRUE(Durin::TryAdd(2, 3, 5, Value));
	EXPECT_EQ(Value, 5);

	Value = 91;
	EXPECT_FALSE(Durin::TryAdd(std::numeric_limits<Durin::uint64>::max(), 1,
		std::numeric_limits<Durin::uint64>::max(), Value));
	EXPECT_EQ(Value, 91);
	EXPECT_FALSE(Durin::TryMultiply(std::numeric_limits<Durin::uint64>::max(), 2,
		std::numeric_limits<Durin::uint64>::max(), Value));
	EXPECT_EQ(Value, 91);
	EXPECT_FALSE(Durin::TryAlignUp(7, 3, 100, Value));
	EXPECT_EQ(Value, 91);
	EXPECT_TRUE(Durin::TryAlignUp(17, 16, 32, Value));
	EXPECT_EQ(Value, 32);
}

TEST(FFailureTests, StoresOptionalDiagnosticsAndReturnsFalse)
{
	std::string Error;
	EXPECT_FALSE(Durin::Fail("pointer diagnostic", &Error));
	EXPECT_EQ(Error, "pointer diagnostic");
	EXPECT_FALSE(Durin::Fail("reference diagnostic", &Error));
	EXPECT_EQ(Error, "reference diagnostic");
	EXPECT_FALSE(Durin::Fail("ignored diagnostic"));
}
