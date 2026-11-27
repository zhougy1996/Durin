#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin;
using namespace Durin::Tests::ObjectState;

namespace
{
	template<typename Edit>
	void CheckArrayHistory(Edit ApplyEdit, const std::vector<int32>& Expected, int32 ExpectedValue = 7)
	{
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "ArrayOwner"));
		const std::vector<int32> Original{1, 2, 3};
		Object->Numbers = Original;
		Object->Value = 7;
		{
			Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Edit array owner"});
			ASSERT_TRUE(Object->Modify());
			ApplyEdit(*Object);
		}
		ASSERT_EQ(Buffer->GetQueueLength(), 1u);
		ASSERT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
		EXPECT_EQ(Object->Numbers, Expected);
		EXPECT_EQ(Object->Value, ExpectedValue);
		for (int Cycle = 0; Cycle < 2; ++Cycle)
		{
			SCOPED_TRACE(Cycle);
			ASSERT_TRUE(Buffer->Undo());
			EXPECT_EQ(Object->Numbers, Original);
			EXPECT_EQ(Object->Value, 7);
			EXPECT_FALSE(Buffer->CanUndo());
			ASSERT_TRUE(Buffer->Redo());
			EXPECT_EQ(Object->Numbers, Expected);
			EXPECT_EQ(Object->Value, ExpectedValue);
			EXPECT_FALSE(Buffer->CanRedo());
			EXPECT_EQ(Buffer->GetQueueLength(), 1u);
		}
	}
}

TEST(FTransactionArrayTests, ConsecutiveHeadInsertions)
{
	CheckArrayHistory([](auto& Owner) {
		Owner.Numbers.insert(Owner.Numbers.begin(), 4);
		Owner.Numbers.insert(Owner.Numbers.begin(), 5);
	}, {5, 4, 1, 2, 3});
}

TEST(FTransactionArrayTests, ConsecutiveHeadRemovals)
{
	CheckArrayHistory([](auto& Owner) {
		Owner.Numbers.erase(Owner.Numbers.begin());
		Owner.Numbers.erase(Owner.Numbers.begin());
	}, {3});
}

TEST(FTransactionArrayTests, RepeatedWritesToSameElement)
{
	CheckArrayHistory([](auto& Owner) {
		Owner.Numbers[1] = 4;
		Owner.Numbers[1] = 5;
	}, {1, 5, 3});
}

TEST(FTransactionArrayTests, StructuralEditsAndSiblingField)
{
	CheckArrayHistory([](auto& Owner) {
		Owner.Numbers.erase(Owner.Numbers.begin());
		Owner.Value = 42;
		Owner.Numbers.insert(Owner.Numbers.begin() + 1, 5);
	}, {2, 5, 3}, 42);
}

TEST(FTransactionArrayTests, WholeObjectCapturesArrayEditAndDeletion)
{
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	auto* Object = NewObject<DTransactionRecordParticipant>(nullptr, "ArrayDeleteOwner");
	Object->Numbers = {1, 2};
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Edit range and delete owner"});
		ASSERT_TRUE(Object->Modify());
		Object->Numbers[1] = 3;
		Object->Value = 42;
		Object->MarkAsGarbage();
	}
	for (int Cycle = 0; Cycle < 2; ++Cycle)
	{
		CollectGarbage(EObjectFlags::Standalone);
		ASSERT_TRUE(Buffer->Undo());
		ASSERT_TRUE(IsValid(Object));
		EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2}));
		ASSERT_TRUE(Buffer->Redo());
		EXPECT_TRUE(Object->IsGarbage());
		EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 3}));
		EXPECT_EQ(Object->Value, 42);
	}
}
