#include "Misc/Name.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint32 NamePoolShardCount = 1 << 10;

	auto GetDisplayShard(std::string_view Name) -> Durin::uint32
	{
		const Durin::uint64 Hash = std::hash<std::string_view>{}(Name);
		return static_cast<Durin::uint32>(Hash >> 32) & (NamePoolShardCount - 1);
	}

	auto MakeNamesAcrossDistinctDisplayShards(size_t Count) -> std::vector<std::string>
	{
		std::array<bool, NamePoolShardCount> UsedShards{};
		std::vector<std::string> Names;
		Names.reserve(Count);

		for (Durin::uint32 Candidate = 0; Names.size() < Count; ++Candidate)
		{
			std::string Name = "FNameDistributedShardValue" + std::to_string(Candidate);
			const Durin::uint32 Shard = GetDisplayShard(Name);
			if (!UsedShards[Shard])
			{
				UsedShards[Shard] = true;
				Names.emplace_back(std::move(Name));
			}
		}

		return Names;
	}

	auto MakeNamesForDisplayShard(Durin::uint32 TargetShard, size_t Count) -> std::vector<std::string>
	{
		std::vector<std::string> Names;
		Names.reserve(Count);

		for (Durin::uint32 Candidate = 0; Names.size() < Count; ++Candidate)
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

	TEST(FNameTests, ConstructsExplicitNumberFromCString)
	{
		const Durin::FName Name("Foo", 3);

		EXPECT_EQ(Name.ToString(), "Foo_3");
		EXPECT_EQ(Name.GetNumber(), 4U);
	}

	TEST(FNameTests, ExplicitNumberConstructorsAreEquivalent)
	{
		const Durin::FName CStringName("Foo", 3);
		const Durin::FName StringViewName(std::string_view("Foo"), 3);

		EXPECT_EQ(CStringName, StringViewName);
		EXPECT_EQ(CStringName.ToString(), StringViewName.ToString());
	}

	TEST(FNameTests, DetectedAndExplicitNumbersAreEquivalent)
	{
		const Durin::FName DetectedName("Foo_3");
		const Durin::FName ExplicitName("Foo", 3);

		EXPECT_EQ(DetectedName, ExplicitName);
		EXPECT_EQ(DetectedName.GetNumber(), ExplicitName.GetNumber());
		EXPECT_EQ(DetectedName.ToString(), ExplicitName.ToString());
	}

	TEST(FNameTests, MinusOneMeansNoNumber)
	{
		const Durin::FName Name("Foo", -1);

		EXPECT_EQ(Name.ToString(), "Foo");
		EXPECT_EQ(Name.GetNumber(), 0U);
	}

	TEST(FNameTests, ExplicitNumberComparisonIgnoresCaseByDefault)
	{
		const Durin::FName UpperName("Foo", 3);
		const Durin::FName LowerName("foo", 3);

		EXPECT_TRUE(UpperName.Equals(LowerName));
		EXPECT_FALSE(UpperName.Equals(LowerName, Durin::ENameCase::CaseSensitive));
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
