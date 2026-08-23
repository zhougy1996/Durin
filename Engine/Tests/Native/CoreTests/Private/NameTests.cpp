#include "Misc/Name.h"

#include <gtest/gtest.h>

namespace
{
	constexpr uint32 NamePoolShardCount = 1 << 10;

	auto GetDisplayShard(std::string_view Name) -> uint32
	{
		const uint64 Hash = std::hash<std::string_view>{}(Name);
		return static_cast<uint32>(Hash >> 32) & (NamePoolShardCount - 1);
	}

	auto MakeNamesAcrossDistinctDisplayShards(size_t Count) -> std::vector<std::string>
	{
		std::array<bool, NamePoolShardCount> UsedShards{};
		std::vector<std::string> Names;
		Names.reserve(Count);

		for (uint32 Candidate = 0; Names.size() < Count; ++Candidate)
		{
			std::string Name = "FNameDistributedShardValue" + std::to_string(Candidate);
			const uint32 Shard = GetDisplayShard(Name);
			if (!UsedShards[Shard])
			{
				UsedShards[Shard] = true;
				Names.emplace_back(std::move(Name));
			}
		}

		return Names;
	}

	auto MakeNamesForDisplayShard(uint32 TargetShard, size_t Count) -> std::vector<std::string>
	{
		std::vector<std::string> Names;
		Names.reserve(Count);

		for (uint32 Candidate = 0; Names.size() < Count; ++Candidate)
		{
			std::string Name = "FNameGrowingShardValue" + std::to_string(Candidate);
			if (GetDisplayShard(Name) == TargetShard)
			{
				Names.emplace_back(std::move(Name));
			}
		}

		return Names;
	}

	auto ExpectNamesRoundTrip(const std::vector<std::string>& Names) -> void
	{
		std::vector<Durin::FName> StoredNames;
		StoredNames.reserve(Names.size());
		for (const std::string& Name : Names)
		{
			StoredNames.emplace_back(Name);
		}

		for (size_t Index = 0; Index < Names.size(); ++Index)
		{
			EXPECT_EQ(StoredNames[Index].ToString(), Names[Index]);
			EXPECT_EQ(Durin::FName(Names[Index]).ToString(), Names[Index]);
		}
	}

	TEST(FNameTests, CanonicalizesNoneAndExplicitOrDetectedNumbers)
	{
		const Durin::FName DefaultName;
		const Durin::FName LiteralNone("None");
		const Durin::FName FirstOrdinaryName("FirstOrdinaryName");

		EXPECT_TRUE(DefaultName.IsNone());
		EXPECT_EQ(DefaultName.ToString(), "None");
		EXPECT_TRUE(LiteralNone.IsNone());
		EXPECT_FALSE(FirstOrdinaryName.IsNone());

		const Durin::FName CStringName("Foo", 3);
		const Durin::FName StringViewName(std::string_view("Foo"), 3);
		EXPECT_EQ(CStringName.ToString(), "Foo_3");
		EXPECT_EQ(CStringName.GetNumber(), 4U);
		EXPECT_EQ(CStringName, StringViewName);
		EXPECT_EQ(CStringName.ToString(), StringViewName.ToString());
		const Durin::FName DetectedName("Foo_3");
		EXPECT_EQ(DetectedName, CStringName);
		EXPECT_EQ(DetectedName.GetNumber(), CStringName.GetNumber());
		EXPECT_EQ(DetectedName.ToString(), CStringName.ToString());

		const Durin::FName UnnumberedName("Foo", -1);
		EXPECT_EQ(UnnumberedName.ToString(), "Foo");
		EXPECT_EQ(UnnumberedName.GetNumber(), 0U);
	}

	TEST(FNameTests, PreservesOutOfRangeNumericSuffixesAsPlainNames)
	{
		const Durin::FName LargestSupported("Bone_2147483646");
		const Durin::FName IntMax("Bone_2147483647");
		const Durin::FName TenDigitOverflow("Bone_9999999999");

		EXPECT_EQ(LargestSupported.GetNumber(), 2147483647U);
		EXPECT_EQ(IntMax.GetNumber(), 0U);
		EXPECT_EQ(TenDigitOverflow.GetNumber(), 0U);
		EXPECT_EQ(IntMax.ToString(), "Bone_2147483647");
		EXPECT_EQ(TenDigitOverflow.ToString(), "Bone_9999999999");
		EXPECT_EQ(Durin::FName("Bone_04").ToString(), "Bone_04");
		EXPECT_EQ(Durin::FName("Bone_12345678901").ToString(), "Bone_12345678901");
	}

	TEST(FNameTests, TruncatesLongNamesAtStoredAndUtf8Boundaries)
	{
		const std::string AtLimit(Durin::FName::MaxSize - 1, 'a');
		const std::string OnePastLimit(Durin::FName::MaxSize, 'b');
		const std::string TwoPastLimit(Durin::FName::MaxSize + 1, 'd');
		const std::string FarPastLimit(Durin::FName::MaxSize * 4, 'c');

		EXPECT_EQ(Durin::FName(AtLimit).ToString(), AtLimit);
		EXPECT_EQ(Durin::FName(OnePastLimit).ToString(), OnePastLimit.substr(0, Durin::FName::MaxSize - 1));
		EXPECT_EQ(Durin::FName(TwoPastLimit).ToString(), TwoPastLimit.substr(0, Durin::FName::MaxSize - 1));
		EXPECT_EQ(Durin::FName(FarPastLimit).ToString(), FarPastLimit.substr(0, Durin::FName::MaxSize - 1));
		const std::string Prefix(Durin::FName::MaxSize - 2, 'a');
		const std::string Name = Prefix + "\xe4\xb8\xad";

		EXPECT_EQ(Durin::FName(Name).ToString(), Prefix);
	}

	TEST(FNameTests, CaseComparisonIncludesNumbersAndPreservesUtf8Bytes)
	{
		const Durin::FName UpperName("MIXED_\xe4\xb8\xad\xe6\x96\x87");
		const Durin::FName LowerName("mixed_\xe4\xb8\xad\xe6\x96\x87");
		const Durin::FName UpperAccented("\xc3\x89");
		const Durin::FName LowerAccented("\xc3\xa9");

		EXPECT_TRUE(UpperName.Equals(LowerName));
		EXPECT_FALSE(UpperName.Equals(LowerName, Durin::ENameCase::CaseSensitive));
		EXPECT_FALSE(UpperAccented.Equals(LowerAccented));
		EXPECT_EQ(UpperName.ToString(), "MIXED_\xe4\xb8\xad\xe6\x96\x87");

		const Durin::FName UpperNumberedName("Foo", 3);
		const Durin::FName LowerNumberedName("foo", 3);

		EXPECT_TRUE(UpperNumberedName.Equals(LowerNumberedName));
		EXPECT_FALSE(UpperNumberedName.Equals(LowerNumberedName, Durin::ENameCase::CaseSensitive));
	}

	TEST(FNameTests, RoutesDisplayEntriesAcrossTheirHashedShards)
	{
		// More entries than one shard can hold would expose accidental routing to
		// shard zero, while the selected hashes keep the intended shards sparse.
		ExpectNamesRoundTrip(MakeNamesAcrossDistinctDisplayShards(300));
	}

	TEST(FNameTests, PreservesEntriesWhenGrowingADisplayShard)
	{
		// A shard grows above 90% of its initial 256 slots.
		ExpectNamesRoundTrip(MakeNamesForDisplayShard(511, 240));
	}
}
