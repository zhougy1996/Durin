#include <gtest/gtest.h>

#include <iostream>
#include <semaphore>

#include "Asset/CookedMeshLoadManager.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "Threading/RunnableThread.h"

namespace
{
	using namespace Durin;
	using namespace Durin;

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
				return std::unexpected(FPackageResourceReadError{.Status = EPackageResourceReadStatus::Cancelled,
					.Reason = EPackageResourceReadReason::Cancelled});
			return FSharedByteBuffer::Take(Durin::FByteBuffer(static_cast<size_t>(Size), std::byte{0x2a}));
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
		if (auto Attached = FBulkData::TryAttach({
			.LogicalSize = 4,
			.Range = {.Resource = Resource, .StoredSize = 4, .Alignment = 1}}); !Attached) ADD_FAILURE() << FormatBulkDataError(Attached.error());
		else { Result = std::move(*Attached); }
		return Result;
	}

	auto MakeRequest(
		DObject& Owner,
		uint64 Generation,
		FBulkData Field,
		bool* bCurrent,
		uint32* PublishCount,
		uint64 RetainedBytes = 4,
		uint32* TerminalCount = nullptr,
		ECookedMeshFamily Family = ECookedMeshFamily::StaticMesh,
		FCookedMeshLoadError* CapturedError = nullptr)
		-> FCookedMeshLoadRequest
	{
		return {
			.Identity = {
				.Owner = FObjectKey(&Owner),
				.Family = Family,
				.LoadGeneration = Generation,
				.ResourceRevision = 1,
				.MetadataIdentity = 7},
			.Fields = {std::move(Field)},
			.Worker = [RetainedBytes](std::span<const FSharedByteBuffer> Buffers,
				const FTaskCancellationToken&) -> FCookedMeshWorkerResult {
				if (Buffers.size() != 1 || Buffers.front().GetSize() != 4)
					return {.Error = {.Code = ECookedMeshLoadError::FieldCount, .Actual = Buffers.size(), .Expected = 1}};
				return {.Product = std::make_unique<FTestProduct>(
					std::to_integer<uint8>(Buffers.front()[0])),
					.RetainedBytes = RetainedBytes};
			},
			.IsCurrent = [bCurrent](const DObject&, const FCookedMeshLoadIdentity&) {
				return *bCurrent;
			},
			.Publish = [PublishCount](DObject&, const FCookedMeshLoadIdentity&,
				std::unique_ptr<ICookedMeshDetachedProduct> Product) -> FCookedMeshLoadResult {
				auto* Typed = dynamic_cast<FTestProduct*>(Product.get());
				if (!Typed || Typed->Value != 0x2a)
				{
					return {.Error = {.Code = ECookedMeshLoadError::InvalidProduct}};
				}
				++*PublishCount;
				return {};
			},
			.OnTerminal = [TerminalCount, CapturedError](DObject&,
				const FCookedMeshLoadIdentity&, ECookedMeshTerminalState,
				const FCookedMeshLoadError& Error) {
				if (CapturedError) *CapturedError = Error;
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
	SaturatesConfiguredCountAndByteBudgetsAcrossConcurrentLifecycleChanges)
{
	InitializeDObjectSystem();
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	const bool bOwnsScheduler = !IsTaskSchedulerRunning();
	if (bOwnsScheduler) ASSERT_TRUE(InitializeTaskScheduler(4));

	// This scale-equivalent fixture makes every request one eighth of the byte
	// budget. It exercises the production 8-way admission ratio without making
	// a correctness test allocate the production 256 MiB ceiling.
	FCookedMeshLoadManager Manager({
		.MaxConcurrentRequests = 8,
		.MaxEstimatedBytes = 32,
		.MaxPendingRequests = 8,
		.MaxPendingEstimatedBytes = 32,
		.MaxPendingCompletionBytes = 32,
		.MaxIoPollsPerPump = 16,
		.MaxCompletionsPerPump = 4});
	ASSERT_TRUE(Manager.Initialize());

	std::array<DObject*, 8> Owners{};
	std::array<std::shared_ptr<FControlledPackageResource>, 8> Resources{};
	bool bCurrent = true;
	uint32 PublishCount = 0;
	uint32 TerminalCount = 0;
	for (uint32 Index = 0; Index < Owners.size(); ++Index)
	{
		Owners[Index] = NewObject<DObject>(nullptr,
			"CookedMeshConcurrentOwner" + std::to_string(Index));
		AddToRoot(Owners[Index]);
		Resources[Index] = std::make_shared<FControlledPackageResource>();
		ASSERT_TRUE(Manager.Submit(MakeRequest(*Owners[Index], 1,
			MakeBulkData(Resources[Index]), &bCurrent, &PublishCount, 4,
			&TerminalCount, ECookedMeshFamily::StaticMesh)));
	}

	const FCookedMeshLoadDiagnostics Saturated = Manager.GetDiagnostics();
	EXPECT_EQ(Saturated.InFlightCount, 8u);
	EXPECT_EQ(Saturated.InFlightEstimatedBytes, 32u);
	const uint32 ExpectedStartedReadCount = std::min<uint32>(
		GetTaskSchedulerDiagnostics().WorkerCount,
		static_cast<uint32>(Resources.size()));
	const bool bExpectedReadsStarted = PumpUntil(Manager, [&] {
		return std::ranges::count_if(Resources,
			[](const auto& Resource) { return Resource->HasStarted(); })
			>= ExpectedStartedReadCount;
	});

	// Exercise reassignment/supersession, unload cancellation, package-resource
	// retirement, and object destruction while static-mesh requests occupy the
	// full configured admission budget.
	auto SuccessorResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(*Owners[0], 2,
		MakeBulkData(SuccessorResource), &bCurrent, &PublishCount, 4,
		&TerminalCount, ECookedMeshFamily::StaticMesh)));
	EXPECT_TRUE(Manager.Cancel(FObjectKey(Owners[1])));
	std::thread RetireThread([Resource = Resources[2]] { Resource->Retire(); });
	RemoveFromRoot(Owners[3]);
	MarkObjectHierarchyAsGarbage(Owners[3]);
	CollectGarbage();
	EXPECT_EQ(Manager.GetDiagnostics().PendingRequestCount, 1u);
	EXPECT_EQ(Manager.GetDiagnostics().PendingRequestEstimatedBytes, 4u);
	const auto RetireDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!Resources[2]->IsRetired()
		&& std::chrono::steady_clock::now() < RetireDeadline)
	{
		std::this_thread::yield();
	}
	const bool bRetirementStarted = Resources[2]->IsRetired();

	for (const auto& Resource : Resources) Resource->Release();
	RetireThread.join();
	const bool bSuccessorStarted =
		PumpUntil(Manager, [&] { return SuccessorResource->HasStarted(); });
	SuccessorResource->Release();
	ASSERT_TRUE(bExpectedReadsStarted);
	ASSERT_TRUE(bRetirementStarted);
	ASSERT_TRUE(bSuccessorStarted);

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < Deadline)
	{
		EXPECT_LE(Manager.Pump(), 4u);
		const FCookedMeshLoadDiagnostics Current = Manager.GetDiagnostics();
		if (Current.InFlightCount == 0 && Current.PendingRequestCount == 0) break;
		std::this_thread::yield();
	}

	Manager.Shutdown();
	const FCookedMeshLoadDiagnostics Final = Manager.GetDiagnostics();
	EXPECT_EQ(Final.State, ECookedMeshManagerState::Stopped);
	EXPECT_EQ(Final.InFlightCount, 0u);
	EXPECT_EQ(Final.PendingRequestCount, 0u);
	EXPECT_EQ(Final.PendingCompletionCount, 0u);
	EXPECT_EQ(Final.InFlightEstimatedBytes, 0u);
	EXPECT_EQ(Final.PendingRequestEstimatedBytes, 0u);
	EXPECT_EQ(Final.PendingCompletionBytes, 0u);
	EXPECT_EQ(Final.PeakInFlightCount, 8u);
	EXPECT_EQ(Final.PeakInFlightEstimatedBytes, 32u);
	EXPECT_EQ(Final.PeakPendingRequestCount, 1u);
	EXPECT_EQ(Final.PeakPendingRequestEstimatedBytes, 4u);
	EXPECT_LE(Final.PeakPendingCompletionCount, 8u);
	EXPECT_LE(Final.PeakPendingCompletionBytes, 32u);
	EXPECT_EQ(Final.AcceptedCount, 9u);
	EXPECT_EQ(Final.SupersededCount, 1u);
	EXPECT_EQ(Final.SucceededCount + Final.FailedCount + Final.CancelledCount
		+ Final.StaleCount, Final.AcceptedCount);
	EXPECT_EQ(PublishCount, Final.SucceededCount);
	EXPECT_GE(TerminalCount, 3u);
	std::cout << "cooked_mesh_qualification"
		<< " read_ready_us=" << Final.ReadReadyMicroseconds
		<< " worker_us=" << Final.WorkerMicroseconds
		<< " game_thread_completion_us="
		<< Final.GameThreadCompletionMicroseconds
		<< " peak_in_flight_count=" << Final.PeakInFlightCount
		<< " peak_in_flight_bytes=" << Final.PeakInFlightEstimatedBytes
		<< " peak_completion_bytes=" << Final.PeakPendingCompletionBytes
		<< '\n';

	for (uint32 Index = 0; Index < Owners.size(); ++Index)
	{
		if (Index == 3) continue;
		RemoveFromRoot(Owners[Index]);
		MarkObjectHierarchyAsGarbage(Owners[Index]);
	}
	CollectGarbage();
	if (bOwnsScheduler) ShutdownTaskScheduler(true);
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
	auto* First = NewObject<DObject>(nullptr, "CookedMeshManagerFirst");
	auto* Second = NewObject<DObject>(nullptr, "CookedMeshManagerSecond");
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
	const auto Rejected = Manager.Submit(MakeRequest(
		*Second, 1, MakeBulkData(RejectedResource), &bCurrent, &PublishCount));
	EXPECT_EQ(Rejected.Error.Code, ECookedMeshAdmissionError::FlightBudget);
	EXPECT_EQ(Rejected.Error.Identity.Owner, FObjectKey(Second));
	EXPECT_EQ(Rejected.Error.RequestCount, 1u);
	EXPECT_EQ(Rejected.Error.RequestLimit, 1u);
	EXPECT_EQ(Rejected.Error.RequestedBytes, 4u);
	EXPECT_EQ(Rejected.Error.ReservedBytes, 4u);
	EXPECT_EQ(Rejected.Error.ByteLimit, 4u);
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
	const auto Older = Manager.Submit(MakeRequest(
		*First, 10, MakeBulkData(OlderResource), &bCurrent, &PublishCount));
	EXPECT_EQ(Older.Error.Code, ECookedMeshAdmissionError::StaleGeneration);
	EXPECT_EQ(Older.Error.Identity.LoadGeneration, 10u);
	EXPECT_EQ(Older.Error.CurrentGeneration, 11u);
	ASSERT_TRUE(Older.Error.ExistingIdentity.has_value());
	EXPECT_EQ(Older.Error.ExistingIdentity->LoadGeneration, 11u);
	auto Conflict = MakeRequest(*First, 11, MakeBulkData(OlderResource), &bCurrent, &PublishCount);
	Conflict.Identity.MetadataIdentity = 99;
	const auto Conflicted = Manager.Submit(std::move(Conflict));
	EXPECT_EQ(Conflicted.Error.Code, ECookedMeshAdmissionError::IdentityConflict);
	EXPECT_EQ(Conflicted.Error.Identity.MetadataIdentity, 99u);
	ASSERT_TRUE(Conflicted.Error.ExistingIdentity.has_value());
	EXPECT_EQ(Conflicted.Error.ExistingIdentity->MetadataIdentity, 7u);
	auto Oversized = MakeRequest(*First, 12, MakeBulkData(OlderResource), &bCurrent, &PublishCount);
	Oversized.Fields.push_back(MakeBulkData(OlderResource));
	const auto PendingRejected = Manager.Submit(std::move(Oversized));
	EXPECT_EQ(PendingRejected.Error.Code, ECookedMeshAdmissionError::PendingBudget);
	EXPECT_EQ(PendingRejected.Error.RequestedBytes, 8u);
	EXPECT_EQ(PendingRejected.Error.ReservedBytes, 0u);
	EXPECT_EQ(PendingRejected.Error.ByteLimit, 4u);
	EXPECT_EQ(PendingRejected.Error.RequestCount, 1u);
	EXPECT_EQ(PendingRejected.Error.RequestLimit, 1u);
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
	FCookedMeshLoadError BudgetError;
	auto OversizedResultResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*First, 3, MakeBulkData(OversizedResultResource),
		&bCurrent, &PublishCount, 5, &TerminalCount, ECookedMeshFamily::StaticMesh, &BudgetError)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return OversizedResultResource->HasStarted(); }));
	OversizedResultResource->Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		return Manager.GetDiagnostics().InFlightCount == 0;
	}));
	EXPECT_EQ(Manager.GetDiagnostics().FailedCount, 1u);
	EXPECT_EQ(PublishCount, 2u);
	EXPECT_EQ(TerminalCount, 1u);
	EXPECT_EQ(BudgetError.Code, ECookedMeshLoadError::CompletionBudget);
	EXPECT_EQ(BudgetError.Owner, FObjectKey(First));
	EXPECT_EQ(BudgetError.Actual, 5u);
	EXPECT_EQ(BudgetError.Expected, 4u);
	EXPECT_EQ(BudgetError.Reserved, 0u);

	// Cancellation reaches the package request and produces one terminal result.
	auto CancelledResource = std::make_shared<FControlledPackageResource>();
	ASSERT_TRUE(Manager.Submit(MakeRequest(
		*Second, 2, MakeBulkData(CancelledResource), &bCurrent, &PublishCount)));
	ASSERT_TRUE(PumpUntil(Manager, [&] { return CancelledResource->HasStarted(); }));
	EXPECT_TRUE(Manager.Cancel(FObjectKey(Second)));
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
	auto* Destroyed = NewObject<DObject>(nullptr, "CookedMeshManagerDestroyed");
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
	EXPECT_EQ(Final.RejectedCount, 4u);
	EXPECT_EQ(Final.SucceededCount + Final.FailedCount + Final.CancelledCount
		+ Final.StaleCount, Final.AcceptedCount);

	RemoveFromRoot(First);
	RemoveFromRoot(Second);
	MarkObjectHierarchyAsGarbage(First);
	MarkObjectHierarchyAsGarbage(Second);
	CollectGarbage();

	if (bOwnsScheduler) ShutdownTaskScheduler(true);
}

TEST(FCookedMeshLoadManagerTests, TerminalRetainsWorkerAndPublisherCauses)
{
	InitializeDObjectSystem();
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	const bool bOwnsScheduler = !IsTaskSchedulerRunning();
	if (bOwnsScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	auto* Owner = NewObject<DObject>(nullptr, "TypedTerminalOwner");
	AddToRoot(Owner);
	const FObjectKey OwnerKey(Owner);
	bool bCurrent = true;
	uint32 PublishCount = 0;
	uint32 TerminalCount = 0;
	FCookedMeshLoadError WorkerError, PublishError, ReadError;
	for (uint64 Generation = 1; Generation <= 3; ++Generation)
	{
		auto Resource = std::make_shared<FControlledPackageResource>();
		auto Request = MakeRequest(*Owner, Generation, MakeBulkData(Resource),
			&bCurrent, &PublishCount, 4, &TerminalCount, ECookedMeshFamily::StaticMesh,
			Generation == 1 ? &WorkerError : Generation == 2 ? &PublishError : &ReadError);
		if (Generation == 1)
			Request.Worker = [](std::span<const FSharedByteBuffer>, const FTaskCancellationToken&) -> FCookedMeshWorkerResult {
				return {.Error = {.Code = ECookedMeshLoadError::Product,
					.ProductCause = std::make_shared<FCookedMeshProductError>(FCookedMeshProductError{
						.Code = ECookedMeshProductError::MaterialSlotCount, .Actual = 7, .Expected = 2})}};
			};
		else if (Generation == 2)
			Request.Publish = [](DObject&, const FCookedMeshLoadIdentity&,
				std::unique_ptr<ICookedMeshDetachedProduct>) -> FCookedMeshLoadResult {
				return {.Error = {.Code = ECookedMeshLoadError::InvalidProduct, .Actual = 9}};
			};
		ASSERT_TRUE(Manager.Submit(std::move(Request)));
		ASSERT_TRUE(PumpUntil(Manager, [&] { return Resource->HasStarted(); }));
		if (Generation == 3) ASSERT_TRUE(Manager.Cancel(OwnerKey));
		Resource->Release();
		ASSERT_TRUE(PumpUntil(Manager, [&] { return Manager.GetDiagnostics().InFlightCount == 0; }));
	}
	EXPECT_EQ(PublishCount, 0u);
	EXPECT_EQ(TerminalCount, 3u);
	Manager.Shutdown();
	RemoveFromRoot(Owner);
	MarkObjectHierarchyAsGarbage(Owner);
	CollectGarbage();
	EXPECT_EQ(WorkerError.Owner, OwnerKey);
	EXPECT_EQ(WorkerError.Code, ECookedMeshLoadError::Product);
	ASSERT_NE(WorkerError.ProductCause, nullptr);
	EXPECT_EQ(WorkerError.ProductCause->Code, ECookedMeshProductError::MaterialSlotCount);
	EXPECT_EQ(WorkerError.ProductCause->Actual, 7u);
	EXPECT_EQ(WorkerError.ProductCause->Expected, 2u);
	EXPECT_EQ(PublishError.Owner, OwnerKey);
	EXPECT_EQ(PublishError.Code, ECookedMeshLoadError::InvalidProduct);
	EXPECT_EQ(PublishError.Actual, 9u);
	EXPECT_EQ(ReadError.Owner, OwnerKey);
	EXPECT_EQ(ReadError.Code, ECookedMeshLoadError::Read);
	EXPECT_EQ(ReadError.Index, 0u);
	ASSERT_NE(ReadError.ReadCause, nullptr);
	ASSERT_FALSE(*ReadError.ReadCause);
	EXPECT_EQ(ReadError.ReadCause->error().Status, EPackageResourceReadStatus::Cancelled);
	const auto& Cause = ReadError.ReadCause->error();
	EXPECT_TRUE(Cause.Reason == EPackageResourceReadReason::Cancelled
		|| Cause.Reason == EPackageResourceReadReason::TaskCancelled);
	if (Cause.Reason == EPackageResourceReadReason::TaskCancelled)
	{
		ASSERT_TRUE(Cause.TaskState.has_value());
		EXPECT_EQ(*Cause.TaskState, ETaskState::Canceled);
	}
	if (bOwnsScheduler) ShutdownTaskScheduler(true);
}

TEST(FCookedMeshLoadManagerTests, AdmissionOwnsInvalidRequestAndFieldContext)
{
	InitializeDObjectSystem();
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	FCookedMeshLoadManager Manager;
	const auto Invalid = Manager.Submit({});
	EXPECT_EQ(Invalid.Error.Code, ECookedMeshAdmissionError::InvalidRequest);
	EXPECT_TRUE(IsObjectKeyNull(Invalid.Error.Identity.Owner));
	EXPECT_EQ(Invalid.Error.FieldCount, 0u);
	EXPECT_FALSE(Invalid.Error.HasWorker);
	EXPECT_FALSE(Invalid.Error.HasPublisher);
	auto* Owner = NewObject<DObject>(nullptr, "AdmissionErrorOwner");
	const FObjectKey Key(Owner);
	bool bCurrent = true;
	uint32 PublishCount = 0;
	auto Empty = MakeRequest(*Owner, 7, FBulkData{}, &bCurrent, &PublishCount);
	const auto FieldError = Manager.Submit(std::move(Empty));
	EXPECT_EQ(FieldError.Error.Code, ECookedMeshAdmissionError::FieldSize);
	EXPECT_EQ(FieldError.Error.Identity.Owner, Key);
	EXPECT_EQ(FieldError.Error.Identity.LoadGeneration, 7u);
	EXPECT_EQ(FieldError.Error.Index, 0u);
	EXPECT_EQ(FieldError.Error.FieldBytes, 0u);
	EXPECT_EQ(FieldError.Error.RequestedBytes, 0u);
	EXPECT_EQ(FieldError.Error.ByteLimit, MaximumBulkDataBytes);
	auto Resource = std::make_shared<FControlledPackageResource>();
	const auto Stopped = Manager.Submit(MakeRequest(*Owner, 8, MakeBulkData(Resource), &bCurrent, &PublishCount));
	EXPECT_EQ(Stopped.Error.Code, ECookedMeshAdmissionError::NotAccepting);
	EXPECT_EQ(Stopped.Error.State, ECookedMeshManagerState::Stopped);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	MarkObjectHierarchyAsGarbage(Owner);
	CollectGarbage();
	EXPECT_EQ(Stopped.Error.Identity.Owner, Key);
	EXPECT_EQ(FieldError.Error.Identity.LoadGeneration, 7u);
}
