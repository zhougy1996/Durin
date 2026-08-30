#include <gtest/gtest.h>

#include <semaphore>

#include "Asset/CookedMeshLoadManager.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Threading/RunnableThread.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	class FControlledPackageResource final : public FPackageResource
	{
	public:
		FControlledPackageResource() : FPackageResource(4) {}

		auto Release() -> void { Gate.release(); }
		auto HasStarted() const -> bool
		{
			return bStarted.load(std::memory_order_acquire);
		}

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool& bCancelled)
			-> FPackageResourceReadResult override
		{
			bStarted.store(true, std::memory_order_release);
			Gate.acquire();
			if (bCancelled.load(std::memory_order_acquire))
				return {.Status = EPackageResourceReadStatus::Cancelled,
					.Message = "controlled cancellation"};
			return {.Status = EPackageResourceReadStatus::Success,
				.Buffer = FSharedByteBuffer::Take(
					std::vector<std::byte>(static_cast<size_t>(Size), std::byte{0x2a}))};
		}

		std::binary_semaphore Gate{0};
		std::atomic_bool bStarted = false;
	};

	struct FTestProduct final : ICookedMeshDetachedProduct
	{
		explicit FTestProduct(uint8 InValue) : Value(InValue) {}
		uint8 Value = 0;
	};

	auto MakeBulkData(const std::shared_ptr<FControlledPackageResource>& Resource)
		-> FBulkData
	{
		FBulkData Result;
		std::string Error;
		if (!FBulkData::TryAttach({
			.LogicalSize = 4,
			.Range = {.Resource = Resource, .StoredSize = 4, .Alignment = 1}},
			Result, &Error)) ADD_FAILURE() << Error;
		return Result;
	}

	auto MakeRequest(
		DObject& Owner,
		uint64 Generation,
		FBulkData Field,
		bool* bCurrent,
		uint32* PublishCount,
		uint64 RetainedBytes = 4,
		uint32* TerminalCount = nullptr) -> FCookedMeshLoadRequest
	{
		return {
			.Identity = {
				.Owner = MakeObjectHandle(&Owner),
				.Family = ECookedMeshFamily::StaticMesh,
				.LoadGeneration = Generation,
				.ResourceRevision = 1,
				.MetadataIdentity = 7},
			.Fields = {std::move(Field)},
			.Worker = [RetainedBytes](std::span<const FSharedByteBuffer> Buffers,
				const FTaskCancellationToken&) -> FCookedMeshWorkerResult {
				if (Buffers.size() != 1 || Buffers.front().GetSize() != 4)
					return {.Message = "unexpected controlled payload"};
				return {.Product = std::make_unique<FTestProduct>(
					std::to_integer<uint8>(Buffers.front()[0])),
					.RetainedBytes = RetainedBytes};
			},
			.IsCurrent = [bCurrent](const DObject&, const FCookedMeshLoadIdentity&) {
				return *bCurrent;
			},
			.Publish = [PublishCount](DObject&, const FCookedMeshLoadIdentity&,
				std::unique_ptr<ICookedMeshDetachedProduct> Product,
				std::string& OutError) {
				auto* Typed = dynamic_cast<FTestProduct*>(Product.get());
				if (!Typed || Typed->Value != 0x2a)
				{
					OutError = "unexpected controlled product";
					return false;
				}
				++*PublishCount;
				return true;
			},
			.OnTerminal = [TerminalCount](DObject&,
				const FCookedMeshLoadIdentity&, ECookedMeshTerminalState,
				std::string_view) {
				if (TerminalCount) ++*TerminalCount;
			},
		};
	}

	template<typename Predicate>
	auto PumpUntil(FCookedMeshLoadManager& Manager, Predicate&& Done) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!Done() && std::chrono::steady_clock::now() < Deadline)
		{
			Manager.Pump();
			std::this_thread::yield();
		}
		return Done();
	}
}

TEST(FCookedMeshLoadManagerTests,
	BoundsCoalescesCancelsRejectsStaleAndDrainsWithoutPublication)
{
	InitializeDObjectSystem();
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	const bool bOwnsScheduler = !IsTaskSchedulerRunning();
	if (bOwnsScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));

	FCookedMeshLoadManager Manager({
		.MaxConcurrentRequests = 1,
		.MaxEstimatedBytes = 4,
		.MaxPendingRequests = 1,
		.MaxPendingEstimatedBytes = 4,
		.MaxPendingCompletionBytes = 4,
		.MaxIoPollsPerPump = 1,
		.MaxCompletionsPerPump = 1});
	ASSERT_TRUE(Manager.Initialize());
	auto* First = NewObject<DMaterial>(nullptr, "CookedMeshManagerFirst");
	auto* Second = NewObject<DMaterial>(nullptr, "CookedMeshManagerSecond");
	AddToRoot(First);
	AddToRoot(Second);
	bool bCurrent = true;
	uint32 PublishCount = 0;
	uint32 TerminalCount = 0;

	auto FirstResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 1, MakeBulkData(FirstResource), &bCurrent, &PublishCount)));
	// An exact current-generation request joins the existing flight before it starts I/O.
	auto CoalescedResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 1, MakeBulkData(CoalescedResource), &bCurrent, &PublishCount)));
	auto RejectedResource = std::make_shared<FControlledPackageResource>();
	EXPECT_FALSE(Manager.Submit(MakeRequest(
		*Second, 1, MakeBulkData(RejectedResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return FirstResource->HasStarted(); }));
	FirstResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(PublishCount, 1u);
	EXPECT_EQ(CoalescedResource->GetReadStats().RequestCount, 0u);

	// A newer generation cancels the old flight and occupies one bounded
	// successor slot until the old I/O reaches its terminal state.
	auto SupersededResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 10, MakeBulkData(SupersededResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return SupersededResource->HasStarted(); }));
	auto NewerResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 11, MakeBulkData(NewerResource), &bCurrent, &PublishCount)));
	auto OlderResource = std::make_shared<FControlledPackageResource>();
	EXPECT_FALSE(Manager.Submit(MakeRequest(
		*First, 10, MakeBulkData(OlderResource), &bCurrent, &PublishCount)));
	EXPECT_EQ(Manager.GetDiagnostics().PendingRequestCount, 1u);
	EXPECT_EQ(NewerResource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(OlderResource->GetReadStats().RequestCount, 0u);
	SupersededResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] { return NewerResource->HasStarted(); }));
	NewerResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(PublishCount, 2u);
	EXPECT_EQ(Manager.GetDiagnostics().SupersededCount, 1u);
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 1u);

	// The full identity predicate is the final publication boundary.
	bCurrent = false;
	auto StaleResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 2, MakeBulkData(StaleResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return StaleResource->HasStarted(); }));
	StaleResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(PublishCount, 2u);
	EXPECT_EQ(Manager.GetDiagnostics().StaleCount, 1u);

	// A dynamically owned candidate must fit the bounded completion mailbox.
	bCurrent = true;
	auto OversizedResultResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 3, MakeBulkData(OversizedResultResource),
		&bCurrent, &PublishCount, 5, &TerminalCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return OversizedResultResource->HasStarted(); }));
	OversizedResultResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(Manager.GetDiagnostics().FailedCount, 1u);
	EXPECT_EQ(PublishCount, 2u);
	EXPECT_EQ(TerminalCount, 1u);

	// Cancellation reaches the package request and produces one terminal result.
	auto CancelledResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*Second, 2, MakeBulkData(CancelledResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return CancelledResource->HasStarted(); }));
	EXPECT_TRUE(Manager.Cancel(MakeObjectHandle(Second)));
	CancelledResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 2u);

	// Package retirement cancels an admitted read and still yields one terminal result.
	auto RetiredResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 4, MakeBulkData(RetiredResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return RetiredResource->HasStarted(); }));
	std::thread RetireThread([RetiredResource] { RetiredResource->Retire(); });
	const auto RetireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!RetiredResource->IsRetired()
		&& std::chrono::steady_clock::now() < RetireDeadline)
		std::this_thread::yield();
	ASSERT_TRUE(RetiredResource->IsRetired());
	RetiredResource->Release();
	RetireThread.join();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 3u);

	// Destruction invalidates the weak object generation before publication.
	auto* Destroyed = NewObject<DMaterial>(nullptr, "CookedMeshManagerDestroyed");
	auto DestroyedResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*Destroyed, 1, MakeBulkData(DestroyedResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return DestroyedResource->HasStarted(); }));
	MarkObjectHierarchyAsGarbage(Destroyed);
	CollectGarbage();
	DestroyedResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(Manager.GetDiagnostics().StaleCount, 2u);

	// Shutdown closes publication before draining package and worker ownership.
	auto ShutdownResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 5, MakeBulkData(ShutdownResource), &bCurrent, &PublishCount,
		4, &TerminalCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return ShutdownResource->HasStarted(); }));
	ShutdownResource->Release();
	Manager.Shutdown();
	const FCookedMeshLoadDiagnostics Final = Manager.GetDiagnostics();
	EXPECT_EQ(Final.State, ECookedMeshManagerState::Stopped);
	EXPECT_EQ(Final.InFlightCount, 0u);
	EXPECT_EQ(Final.PendingRequestCount, 0u);
	EXPECT_EQ(Final.PendingCompletionCount, 0u);
	EXPECT_EQ(Final.InFlightEstimatedBytes, 0u);
	EXPECT_EQ(Final.PendingRequestEstimatedBytes, 0u);
	EXPECT_EQ(PublishCount, 2u);
	EXPECT_EQ(TerminalCount, 2u);
	EXPECT_EQ(Final.AcceptedCount, 9u);
	EXPECT_EQ(Final.CoalescedCount, 1u);
	EXPECT_EQ(Final.SupersededCount, 1u);
	EXPECT_EQ(Final.RejectedCount, 2u);
	EXPECT_EQ(Final.SucceededCount + Final.FailedCount + Final.CancelledCount
		+ Final.StaleCount, Final.AcceptedCount);

	RemoveFromRoot(First);
	RemoveFromRoot(Second);
	MarkObjectHierarchyAsGarbage(First);
	MarkObjectHierarchyAsGarbage(Second);
	CollectGarbage();

	if (bOwnsScheduler) ShutdownTaskScheduler(true);
}
