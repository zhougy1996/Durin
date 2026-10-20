#include "Misc/ITransaction.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/TransactionObjectEvent.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	// A runtime domain can describe an object change without an editor link edge.
	class FRuntimeChange final : public FCommandChange
	{
	public:
		FRuntimeChange(DObject* InTarget, int& InValue) : Target(InTarget), Value(InValue) {}
		auto Apply() -> FCommandChangeResult override { Value = 9; return {}; }
		auto Revert() -> FCommandChangeResult override { Value = 0; return {}; }
		auto GetDescription() const -> std::string_view override { return "Runtime transaction contract"; }
		auto GetModifiedObjects() const -> std::vector<DObject*> override { return {Target}; }
	private:
		DObject* Target;
		int& Value;
	};
}

TEST(FRuntimeTransactionContractTests, RuntimeChangeAndObjectContractsNeedNoEditor)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DObject> Object(NewObject<DObject>(nullptr, "RuntimeTransactionTarget"));
	int Value = 0;
	std::unique_ptr<FCommandChange> Change = std::make_unique<FRuntimeChange>(Object.Get(), Value);
	EXPECT_EQ(Change->GetModifiedObjects(), (std::vector<DObject*>{Object.Get()}));
	ASSERT_TRUE(Change->Apply());
	EXPECT_EQ(Value, 9);
	ASSERT_TRUE(Change->Revert());
	EXPECT_EQ(Value, 0);
	EXPECT_FALSE(Object->FindOrCreateTransactionAnnotation());
	Object->PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation>{});
	Object->PostTransacted(FTransactionObjectEvent{});
}

TEST(FRuntimeTransactionContractTests, OperationIdentitiesRemainDistinctAcrossDomains)
{
	const auto First = AllocateTransactionOperationId();
	const auto Second = AllocateTransactionOperationId();
	EXPECT_NE(First, 0u);
	EXPECT_GT(Second, First);
}

TEST(FRuntimeTransactionContractTests, ExactReferenceDoesNotResolveGarbage)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DObject> Object(NewObject<DObject>(nullptr, "RuntimeExactIdentity"));
	const FPersistentObjectRef Reference(Object.Get());
	EXPECT_EQ(Reference.Resolve(), Object.Get());
	MarkAsGarbage(Object.Get());
	EXPECT_EQ(Reference.Resolve(), nullptr);
	EXPECT_TRUE(Reference.IsStale());
}
