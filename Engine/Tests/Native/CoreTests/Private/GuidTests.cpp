#include "Misc/Guid.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FGuidTests, InvalidStateAndInvalidationUseTheZeroIdentity)
	{
		Durin::FGuid Guid;

		EXPECT_FALSE(Guid.IsValid());
		EXPECT_EQ(Guid.ToString(), "00000000-0000-0000-0000-000000000000");

		Guid = Durin::FGuid(1, 2, 3, 4);
		Guid.Invalidate();
		EXPECT_FALSE(Guid.IsValid());
		EXPECT_EQ(Guid, Durin::FGuid());
	}

	TEST(FGuidTests, CanonicalTextRoundTripsAndRejectsMalformedInputTransactionally)
	{
		const Durin::FGuid Expected(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
		Durin::FGuid Parsed;

		EXPECT_EQ(Expected.ToString(), "00112233-4455-6677-8899-aabbccddeeff");
		EXPECT_TRUE(Durin::FGuid::Parse("00112233-4455-6677-8899-aabbccddeeff", Parsed));
		EXPECT_EQ(Parsed, Expected);

		ASSERT_TRUE(Durin::FGuid::Parse("00112233-4455-6677-8899-AABBCCDDEEFF", Parsed));
		EXPECT_EQ(Parsed.ToString(), "00112233-4455-6677-8899-aabbccddeeff");

		const Durin::FGuid Sentinel(1, 2, 3, 4);
		const std::array<std::string_view, 5> InvalidValues{
			"00112233445566778899aabbccddeeff",
			"{00112233-4455-6677-8899-aabbccddeeff}",
			"00112233_4455-6677-8899-aabbccddeeff",
			"00112233-4455-6677-8899-aabbccddeefg",
			"00112233-4455-6677-8899-aabbccddee"
		};

		for (std::string_view Text : InvalidValues)
		{
			Durin::FGuid Output = Sentinel;
			EXPECT_FALSE(Durin::FGuid::Parse(Text, Output));
			EXPECT_EQ(Output, Sentinel);
		}
	}

	TEST(FGuidTests, GeneratedIdentitiesAreOrderedHashableUniqueVersionFourValues)
	{
		const Durin::FGuid First(1, 2, 3, 4);
		const Durin::FGuid Second(1, 2, 3, 5);
		std::unordered_set<Durin::FGuid> Values{First, Second, First};

		EXPECT_LT(First, Second);
		EXPECT_EQ(Values.size(), 2);
		EXPECT_TRUE(Values.contains(First));

		Values.clear();
		for (size_t Index = 0; Index < 128; ++Index)
		{
			const Durin::FGuid Guid = Durin::FGuid::NewGuid();
			EXPECT_TRUE(Guid.IsValid());
			EXPECT_EQ((Guid.B >> 12) & 0xfu, 4u);
			EXPECT_EQ(Guid.C >> 30, 2u);
			EXPECT_EQ(Guid.ToString()[14], '4');
			EXPECT_TRUE(Guid.ToString()[19] == '8' || Guid.ToString()[19] == '9'
				|| Guid.ToString()[19] == 'a' || Guid.ToString()[19] == 'b');
			EXPECT_TRUE(Values.insert(Guid).second);
		}
	}
}
