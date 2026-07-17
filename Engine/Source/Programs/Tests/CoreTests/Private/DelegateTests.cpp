#include <gtest/gtest.h>

#include "Delegates/Delegate.h"

namespace Durin
{
	namespace
	{
		auto AddOne(int Value) -> int
		{
			return Value + 1;
		}

		std::vector<int>* GStaticBroadcastValues = nullptr;

		auto RecordStaticValue(int Value) -> void
		{
			GStaticBroadcastValues->push_back(Value);
		}

		class FDelegateTestReceiver
		{
		public:
			auto Add(int Value) -> int
			{
				Total += Value;
				return Total;
			}

			auto AddConst(int Value) const -> int
			{
				return Total + Value;
			}

			auto Record(int Value) -> void
			{
				Values.push_back(Value);
			}

			auto RecordConst(int Value) const -> void
			{
				ConstValues.push_back(Value);
			}

			int Total = 0;
			std::vector<int> Values;
			mutable std::vector<int> ConstValues;
		};

		class FSelfReleasingReceiver
		{
		public:
			~FSelfReleasingReceiver()
			{
				*bDestroyed = true;
			}

			auto ReleaseOwner() -> void
			{
				Owner->reset();
				*bObservedAliveAfterRelease = !*bDestroyed;
				*bCallbackCompleted = true;
			}

			std::shared_ptr<FSelfReleasingReceiver>* Owner = nullptr;
			bool* bDestroyed = nullptr;
			bool* bObservedAliveAfterRelease = nullptr;
			bool* bCallbackCompleted = nullptr;
		};

		struct FMoveOnlyValue
		{
			FMoveOnlyValue() = default;
			FMoveOnlyValue(const FMoveOnlyValue&) = delete;
			FMoveOnlyValue(FMoveOnlyValue&&) = default;
		};

		static_assert(Private::TIsValidMulticastSignatureV<void()>);
		static_assert(Private::TIsValidMulticastSignatureV<void(int)>);
		static_assert(Private::TIsValidMulticastSignatureV<void(const FMoveOnlyValue&)>);
		static_assert(!Private::TIsValidMulticastSignatureV<int()>);
		static_assert(!Private::TIsValidMulticastSignatureV<void(FMoveOnlyValue)>);
		static_assert(!Private::TIsValidMulticastSignatureV<void(FMoveOnlyValue&&)>);

		DECLARE_DELEGATE_RetVal_OneParam(int, FDeclaredDelegate, int)
		DECLARE_MULTICAST_DELEGATE_OneParam(FDeclaredMulticastDelegate, int)
		DECLARE_TS_DELEGATE_OneParam(FDeclaredThreadSafeDelegate, int)
		DECLARE_TS_MULTICAST_DELEGATE_OneParam(FDeclaredThreadSafeMulticastDelegate, int)

		static_assert(std::is_same_v<FDeclaredDelegate, TDelegate<int(int)>>);
		static_assert(std::is_same_v<FDeclaredMulticastDelegate, TMulticastDelegate<void(int)>>);
		static_assert(std::is_same_v<FDeclaredThreadSafeDelegate, TThreadSafeDelegate<void(int)>>);
		static_assert(std::is_same_v<FDeclaredThreadSafeMulticastDelegate, TThreadSafeMulticastDelegate<void(int)>>);
	}

	TEST(FDelegateHandleTests, DefaultHandleIsInvalidAndGeneratedHandlesAreUnique)
	{
		FDelegateHandle InvalidHandle;
		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_EQ(0u, InvalidHandle.GetId());

		FDelegateHandle FirstHandle = FDelegateHandle::GenerateNewHandle();
		FDelegateHandle SecondHandle = FDelegateHandle::GenerateNewHandle();
		EXPECT_TRUE(FirstHandle.IsValid());
		EXPECT_TRUE(SecondHandle.IsValid());
		EXPECT_NE(FirstHandle, SecondHandle);

		FirstHandle.Reset();
		EXPECT_FALSE(FirstHandle.IsValid());
		EXPECT_EQ(FirstHandle, InvalidHandle);
	}

	TEST(FDelegateTests, SupportsStaticRawConstAndLambdaBindings)
	{
		TDelegate<int(int)> Delegate;
		Delegate.BindStatic(&AddOne);
		EXPECT_TRUE(Delegate.IsBound());
		EXPECT_EQ(3, Delegate.Execute(2));

		FDelegateTestReceiver Receiver;
		Delegate.BindRaw(&Receiver, &FDelegateTestReceiver::Add);
		EXPECT_EQ(4, Delegate.Execute(4));
		Delegate.BindRaw(&Receiver, &FDelegateTestReceiver::AddConst);
		EXPECT_EQ(7, Delegate.Execute(3));

		Delegate.BindLambda([](int Value) { return Value * 2; });
		EXPECT_EQ(10, Delegate.Execute(5));
		Delegate.Unbind();
		EXPECT_FALSE(Delegate.IsBound());
	}

	TEST(FDelegateTests, ExecuteIfBoundReportsWhetherVoidCallbackRan)
	{
		TDelegate<void(int)> Delegate;
		int Value = 0;
		EXPECT_FALSE(Delegate.ExecuteIfBound(3));

		Delegate.BindLambda([&Value](int InValue) { Value = InValue; });
		EXPECT_TRUE(Delegate.ExecuteIfBound(7));
		EXPECT_EQ(7, Value);
	}

	TEST(FDelegateTests, SharedPointerBindingPinsAndExpiresWithObject)
	{
		TDelegate<void(int)> Delegate;
		auto Receiver = std::make_shared<FDelegateTestReceiver>();
		Delegate.BindSP(Receiver, &FDelegateTestReceiver::Record);
		EXPECT_TRUE(Delegate.ExecuteIfBound(1));
		EXPECT_EQ(std::vector<int>{ 1 }, Receiver->Values);

		Receiver.reset();
		EXPECT_FALSE(Delegate.IsBound());
		EXPECT_FALSE(Delegate.ExecuteIfBound(2));
	}

	TEST(FDelegateTests, SupportsConstSharedPointerBinding)
	{
		TDelegate<int(int)> Delegate;
		auto Receiver = std::make_shared<FDelegateTestReceiver>();
		Receiver->Total = 4;
		Delegate.BindSP(Receiver, &FDelegateTestReceiver::AddConst);
		EXPECT_EQ(7, Delegate.Execute(3));
	}

	TEST(FDelegateTests, SharedPointerBindingPinsObjectUntilCallbackReturns)
	{
		TDelegate<void()> Delegate;
		bool bDestroyed = false;
		bool bObservedAliveAfterRelease = false;
		bool bCallbackCompleted = false;
		auto Receiver = std::make_shared<FSelfReleasingReceiver>();
		Receiver->Owner = &Receiver;
		Receiver->bDestroyed = &bDestroyed;
		Receiver->bObservedAliveAfterRelease = &bObservedAliveAfterRelease;
		Receiver->bCallbackCompleted = &bCallbackCompleted;
		Delegate.BindSP(Receiver, &FSelfReleasingReceiver::ReleaseOwner);

		EXPECT_TRUE(Delegate.ExecuteIfBound());
		EXPECT_TRUE(bCallbackCompleted);
		EXPECT_TRUE(bObservedAliveAfterRelease);
		EXPECT_TRUE(bDestroyed);
		EXPECT_FALSE(Delegate.IsBound());
	}

	TEST(FDelegateTests, CopyAndMoveCreateExpectedIndependentState)
	{
		TDelegate<int(int)> Original;
		Original.BindLambda([](int Value) { return Value + 10; });
		TDelegate<int(int)> Copy = Original;
		Original.Unbind();
		EXPECT_TRUE(Copy.IsBound());
		EXPECT_EQ(12, Copy.Execute(2));

		TDelegate<int(int)> Moved = std::move(Copy);
		EXPECT_FALSE(Copy.IsBound());
		EXPECT_EQ(13, Moved.Execute(3));
	}

	TEST(FMulticastDelegateTests, PreservesOrderAndSupportsEveryBindingKind)
	{
		TMulticastDelegate<void(int)> Delegate;
		std::vector<int> StaticValues;
		GStaticBroadcastValues = &StaticValues;
		FDelegateTestReceiver RawReceiver;
		auto SharedReceiver = std::make_shared<FDelegateTestReceiver>();
		std::vector<int> Order;

		Delegate.AddStatic(&RecordStaticValue);
		Delegate.AddRaw(&RawReceiver, &FDelegateTestReceiver::Record);
		Delegate.AddSP(SharedReceiver, &FDelegateTestReceiver::RecordConst);
		Delegate.AddLambda([&Order](int Value) { Order.push_back(Value); });
		Delegate.Broadcast(5);

		EXPECT_EQ(std::vector<int>{ 5 }, StaticValues);
		EXPECT_EQ(std::vector<int>{ 5 }, RawReceiver.Values);
		EXPECT_EQ(std::vector<int>{ 5 }, SharedReceiver->ConstValues);
		EXPECT_EQ(std::vector<int>{ 5 }, Order);
		GStaticBroadcastValues = nullptr;
	}

	TEST(FMulticastDelegateTests, RemovesByHandleAndObjectAndCanClear)
	{
		TMulticastDelegate<void(int)> Delegate;
		FDelegateTestReceiver FirstReceiver;
		FDelegateTestReceiver SecondReceiver;
		const FDelegateHandle FirstHandle = Delegate.AddRaw(&FirstReceiver, &FDelegateTestReceiver::Record);
		Delegate.AddRaw(&FirstReceiver, &FDelegateTestReceiver::Record);
		Delegate.AddRaw(&SecondReceiver, &FDelegateTestReceiver::Record);

		EXPECT_EQ(3u, Delegate.Num());
		EXPECT_TRUE(Delegate.Remove(FirstHandle));
		EXPECT_FALSE(Delegate.Remove(FirstHandle));
		EXPECT_EQ(1u, Delegate.RemoveAll(&FirstReceiver));
		Delegate.Broadcast(2);
		EXPECT_TRUE(FirstReceiver.Values.empty());
		EXPECT_EQ(std::vector<int>{ 2 }, SecondReceiver.Values);

		Delegate.Clear();
		EXPECT_FALSE(Delegate.IsBound());
		EXPECT_EQ(0u, Delegate.Num());
	}

	TEST(FMulticastDelegateTests, ExpiredWeakBindingsAreSkippedAndCompacted)
	{
		TMulticastDelegate<void(int)> Delegate;
		auto Receiver = std::make_shared<FDelegateTestReceiver>();
		Delegate.AddSP(Receiver, &FDelegateTestReceiver::Record);
		EXPECT_EQ(1u, Delegate.Num());

		Receiver.reset();
		EXPECT_FALSE(Delegate.IsBound());
		Delegate.Broadcast(1);
		EXPECT_EQ(0u, Delegate.Num());
	}

	TEST(FMulticastDelegateTests, MutationsDuringBroadcastAffectTheNextBroadcast)
	{
		TMulticastDelegate<void()> Delegate;
		std::vector<int> Calls;
		FDelegateHandle SecondHandle;
		bool bAddedThird = false;

		Delegate.AddLambda([&]() {
			Calls.push_back(1);
			Delegate.Remove(SecondHandle);
			if (!bAddedThird)
			{
				bAddedThird = true;
				Delegate.AddLambda([&Calls]() { Calls.push_back(3); });
			}
		});
		SecondHandle = Delegate.AddLambda([&Calls]() { Calls.push_back(2); });

		Delegate.Broadcast();
		EXPECT_EQ((std::vector<int>{ 1, 2 }), Calls);
		Calls.clear();
		Delegate.Broadcast();
		EXPECT_EQ((std::vector<int>{ 1, 3 }), Calls);
	}

	TEST(FMulticastDelegateTests, NestedBroadcastObservesLatestState)
	{
		TMulticastDelegate<void(int)> Delegate;
		std::vector<int> Calls;
		Delegate.AddLambda([&](int Depth) {
			Calls.push_back(Depth * 10 + 1);
			if (Depth == 0)
			{
				Delegate.AddLambda([&Calls](int NestedDepth) { Calls.push_back(NestedDepth * 10 + 3); });
				Delegate.Broadcast(1);
			}
		});
		Delegate.AddLambda([&Calls](int Depth) { Calls.push_back(Depth * 10 + 2); });

		Delegate.Broadcast(0);
		EXPECT_EQ((std::vector<int>{ 1, 11, 12, 13, 2 }), Calls);
	}

	TEST(FMulticastDelegateTests, ClearDuringBroadcastOnlyAffectsLaterBroadcasts)
	{
		TMulticastDelegate<void()> Delegate;
		std::vector<int> Calls;
		Delegate.AddLambda([&]() {
			Calls.push_back(1);
			Delegate.Clear();
		});
		Delegate.AddLambda([&Calls]() { Calls.push_back(2); });

		Delegate.Broadcast();
		EXPECT_EQ((std::vector<int>{ 1, 2 }), Calls);
		Delegate.Broadcast();
		EXPECT_EQ((std::vector<int>{ 1, 2 }), Calls);
	}

	TEST(FMulticastDelegateTests, MoveTransfersListenersAndHandles)
	{
		TMulticastDelegate<void()> Source;
		int Calls = 0;
		const FDelegateHandle Handle = Source.AddLambda([&Calls]() { ++Calls; });
		TMulticastDelegate<void()> Destination = std::move(Source);

		EXPECT_EQ(0u, Source.Num());
		Destination.Broadcast();
		EXPECT_EQ(1, Calls);
		EXPECT_TRUE(Destination.Remove(Handle));
	}

	TEST(FThreadSafeDelegateTests, ConcurrentMutationAndBroadcastCompleteWithoutDeadlock)
	{
		TThreadSafeMulticastDelegate<void()> Delegate;
		std::atomic<int> Calls = 0;
		std::atomic<bool> Start = false;
		constexpr int IterationCount = 200;
		Delegate.AddLambda([&Calls]() { Calls.fetch_add(1, std::memory_order_relaxed); });

		std::thread Broadcaster([&]() {
			while (!Start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (int Index = 0; Index < IterationCount; ++Index) Delegate.Broadcast();
		});
		std::thread Mutator([&]() {
			Start.store(true, std::memory_order_release);
			for (int Index = 0; Index < IterationCount; ++Index)
			{
				const FDelegateHandle Handle = Delegate.AddLambda([&Calls]() { Calls.fetch_add(1, std::memory_order_relaxed); });
				if ((Index % 2) == 0) Delegate.Remove(Handle);
			}
		});

		Broadcaster.join();
		Mutator.join();
		EXPECT_GE(Calls.load(std::memory_order_relaxed), IterationCount);
	}
}
