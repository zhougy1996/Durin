#include <gtest/gtest.h>

#include "Templates/AtomicSharedPtr.h"
#include <thread>
#include <vector>

namespace Durin
{
	TEST(AtomicSharedPtrTests, ExchangeTransfersPreviousOwner)
	{
		auto Initial = std::make_shared<int>(1);
		std::weak_ptr<int> Lifetime = Initial;
		TAtomicSharedPtr<int> Value(std::move(Initial));
		auto Previous = Value.Exchange(std::make_shared<int>(2));
		EXPECT_EQ(*Previous, 1);
		EXPECT_EQ(*Value.Load(), 2);
		EXPECT_FALSE(Lifetime.expired());
		Previous.reset();
		EXPECT_TRUE(Lifetime.expired());
	}

	TEST(AtomicSharedPtrTests, CompareExchangeChecksOwnershipAndRefreshesExpected)
	{
		int Storage = 1;
		auto Owner = std::shared_ptr<int>(&Storage, [](int*) {});
		auto Expected = std::shared_ptr<int>(&Storage, [](int*) {});
		TAtomicSharedPtr<int> Value(Owner);
		auto Desired = std::make_shared<int>(2);
		EXPECT_FALSE(Value.CompareExchange(Expected, Desired));
		EXPECT_FALSE(Expected.owner_before(Owner));
		EXPECT_FALSE(Owner.owner_before(Expected));
		EXPECT_TRUE(Value.CompareExchange(Expected, Desired));
		EXPECT_EQ(Expected, Owner);
		EXPECT_EQ(Value.Load(), Desired);
	}

	TEST(AtomicSharedPtrTests, FailedCompareExchangeReleasesExpectedOutsideLock)
	{
		TAtomicSharedPtr<int> Value(std::make_shared<int>(1));
		bool Released = false;
		auto Expected = std::shared_ptr<int>(new int(2), [&](int* Old) {
			EXPECT_EQ(*Value.Load(), 1);
			Released = true;
			delete Old;
		});
		EXPECT_FALSE(Value.CompareExchange(Expected, {}));
		EXPECT_TRUE(Released);
		EXPECT_EQ(Expected, Value.Load());
	}

	TEST(AtomicSharedPtrTests, ConcurrentCompareExchangePreservesEveryUpdate)
	{
		TAtomicSharedPtr<const int> Value(std::make_shared<const int>(0));
		std::vector<std::jthread> Workers;
		for (int Index = 0; Index < 4; ++Index)
			Workers.emplace_back([&] {
				for (int Update = 0; Update < 100; ++Update)
				{
					auto Expected = Value.Load();
					while (!Value.CompareExchange(Expected, std::make_shared<const int>(*Expected + 1))) {}
				}
			});
		Workers.clear();
		EXPECT_EQ(*Value.Load(), 400);
	}
}
