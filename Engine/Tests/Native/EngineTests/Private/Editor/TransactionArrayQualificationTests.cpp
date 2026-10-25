#include "Editor/TransactionRecordTestSupport.h"
#include "DObject/TransactionArray.h"
#include "DObject/TransactionSerialization.h"
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace Durin;
using namespace Durin::Editor;
using namespace Durin::Tests::ObjectState;

namespace
{
#if defined(_MSC_VER) && defined(_DEBUG)
	thread_local size_t AllocationCount = 0;
	thread_local bool bCountAllocations = false;
	int CountAllocation(int Type, void*, size_t, int, long, const unsigned char*, int)
	{
		if (bCountAllocations && (Type == _HOOK_ALLOC || Type == _HOOK_REALLOC)) ++AllocationCount;
		return TRUE;
	}
	struct FAllocationProbe
	{
		FAllocationProbe() : Previous(_CrtSetAllocHook(CountAllocation)) { AllocationCount = 0; bCountAllocations = true; }
		~FAllocationProbe() { bCountAllocations = false; _CrtSetAllocHook(Previous); }
		_CRT_ALLOC_HOOK Previous;
	};
#endif
}

TEST(FTransactionArrayQualificationTests, FixedRangeCaptureCostsDoNotScaleWithContainerLength)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeCost"));
	const auto* Member = Object->GetClass()->FindPropertyByName("Numbers");
	for (auto Operation : {ETransactionArrayOperation::Insert, ETransactionArrayOperation::Remove, ETransactionArrayOperation::Replace})
	{
		size_t PreviousBytes = 0;
			size_t PreviousAllocations = 0;
		for (size_t Num : {10000u, 100000u})
		{
			Object->Numbers.assign(Num, 5);
			const FTransactionArrayCapture Request{.Object = Object.Get(), .Member = Member, .Index = Num / 2, .Count = 1, .Operation = Operation};
			ASSERT_TRUE(FTransactionArrayRecord::Capture(Request)); // Warm metadata and serializer paths.
			const auto Begin = std::chrono::steady_clock::now();
#if defined(_MSC_VER) && defined(_DEBUG)
			std::optional<FAllocationProbe> Probe(std::in_place);
#endif
			auto Record = FTransactionArrayRecord::Capture(Request);
#if defined(_MSC_VER) && defined(_DEBUG)
			const size_t Allocations = AllocationCount; Probe.reset();
#else
			const size_t Allocations = 0;
#endif
			const auto CaptureNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin).count();
			ASSERT_TRUE(Record) << Record.error();
			size_t Bytes = 0; ASSERT_TRUE(Record->TryGetAllocatedSize(Bytes));
			if (PreviousBytes) EXPECT_EQ(Bytes, PreviousBytes);
			PreviousBytes = Bytes;
			if (PreviousAllocations) EXPECT_LE(Allocations, PreviousAllocations + 2);
			PreviousAllocations = Allocations;
			if (Operation == ETransactionArrayOperation::Insert) Object->Numbers.insert(Object->Numbers.begin() + Num / 2, 9);
			else if (Operation == ETransactionArrayOperation::Remove) Object->Numbers.erase(Object->Numbers.begin() + Num / 2);
			else Object->Numbers[Num / 2] = 9;
			const auto ReplayBegin = std::chrono::steady_clock::now();
#if defined(_MSC_VER) && defined(_DEBUG)
			Probe.emplace();
#endif
			ASSERT_TRUE(Record->Apply(false, EPropertyChangeOrigin::Undo, nullptr)); Record->CommitReplay(); Record->ClearPreparedReplay();
			ASSERT_TRUE(Record->Apply(false, EPropertyChangeOrigin::Redo, nullptr)); Record->CommitReplay(); Record->ClearPreparedReplay();
#if defined(_MSC_VER) && defined(_DEBUG)
			const size_t ReplayAllocations = AllocationCount; Probe.reset();
#else
			const size_t ReplayAllocations = 0;
#endif
			const auto ReplayNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ReplayBegin).count();
			const auto FullBegin = std::chrono::steady_clock::now();
#if defined(_MSC_VER) && defined(_DEBUG)
			Probe.emplace();
#endif
			auto Full = CaptureSerializedObject(*Object);
#if defined(_MSC_VER) && defined(_DEBUG)
			const size_t FullAllocations = AllocationCount; Probe.reset();
#else
			const size_t FullAllocations = 0;
#endif
			const auto FullNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - FullBegin).count();
			ASSERT_TRUE(Full); size_t FullBytes = 0; ASSERT_TRUE(Full->TryGetAllocatedSize(FullBytes));
			EXPECT_LT(Bytes, FullBytes);
			std::cout << "ARRAY_COST operation=" << static_cast<int>(Operation) << " N=" << Num << " range_bytes=" << Bytes
				<< " capture_allocations=" << Allocations << " capture_ns=" << CaptureNs << " undo_redo_allocations=" << ReplayAllocations << " undo_redo_ns=" << ReplayNs
				<< " whole_bytes=" << FullBytes << " whole_capture_allocations=" << FullAllocations << " whole_capture_ns=" << FullNs << '\n';
			RecordProperty(std::format("array_cost_{}_{}", static_cast<int>(Operation), Num),
				std::format("range_bytes={} capture_allocations={} capture_ns={} undo_redo_allocations={} undo_redo_ns={} whole_bytes={} whole_capture_allocations={} whole_capture_ns={}",
					Bytes, Allocations, CaptureNs, ReplayAllocations, ReplayNs, FullBytes, FullAllocations, FullNs));
		}
	}
}
