#include <gtest/gtest.h>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <new>

#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageResource.h"
#include "DObject/DObjectGlobals.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include "Texture/Texture2DCompilation.h"

namespace
{
	std::atomic<bool> GCountAllocations = false;
	// Slots are never reused: late destruction from an earlier batch cannot
	// subtract from a later batch's live-byte counter.
	struct FAllocationCounters
	{
		std::atomic<uint64> Count = 0;
		std::atomic<uint64> AllocatedBytes = 0;
		std::atomic<uint64> LiveBytes = 0;
		std::atomic<uint64> PeakLiveBytes = 0;
	};
	std::array<FAllocationCounters, 128> GAllocationCounters;
	std::atomic<uint64> GAllocationEpoch = 0;

	struct alignas(std::max_align_t) FAllocationHeader
	{
		std::size_t Size;
		uint64 Epoch;
	};

	auto FreeAllocation(void* Memory) noexcept -> void
	{
		if (!Memory) return;
		auto* Header = static_cast<FAllocationHeader*>(Memory) - 1;
		if (Header->Epoch != 0)
			GAllocationCounters[Header->Epoch].LiveBytes.fetch_sub(Header->Size);
		std::free(Header);
	}
}

// Qualification-only replacement counts ordinary C++ new calls in this process.
// malloc/realloc and over-aligned new are excluded. Epoch-tagged headers measure
// peak live requested bytes without retaining an allocation map or payloads.
auto operator new(std::size_t Size) -> void*
{
	if (Size > std::numeric_limits<std::size_t>::max() - sizeof(FAllocationHeader))
		throw std::bad_alloc();
	auto* Header = static_cast<FAllocationHeader*>(
		std::malloc(sizeof(FAllocationHeader) + std::max(Size, std::size_t{1})));
	if (!Header) throw std::bad_alloc();
	Header->Size = Size;
	Header->Epoch = 0;
	if (GCountAllocations.load(std::memory_order_relaxed))
	{
		Header->Epoch = GAllocationEpoch.load();
		auto& Counters = GAllocationCounters[Header->Epoch];
		Counters.Count.fetch_add(1, std::memory_order_relaxed);
		Counters.AllocatedBytes.fetch_add(Size, std::memory_order_relaxed);
		const uint64 Live = Counters.LiveBytes.fetch_add(Size) + Size;
		uint64 Peak = Counters.PeakLiveBytes.load();
		while (Live > Peak && !Counters.PeakLiveBytes.compare_exchange_weak(Peak, Live)) {}
	}
	return Header + 1;
}
auto operator new[](std::size_t Size) -> void* { return ::operator new(Size); }
auto operator delete(void* Memory) noexcept -> void { FreeAllocation(Memory); }
auto operator delete[](void* Memory) noexcept -> void { FreeAllocation(Memory); }
auto operator delete(void* Memory, std::size_t) noexcept -> void { FreeAllocation(Memory); }
auto operator delete[](void* Memory, std::size_t) noexcept -> void { FreeAllocation(Memory); }

namespace
{
	using namespace Durin;
	using FClock = std::chrono::steady_clock;
	constexpr uint32 BatchSize = 4;
	constexpr uint32 WarmupBatches = 3;
	constexpr uint32 MeasuredBatches = 30;
	constexpr uint64 RangeBytes = 64 * 1024;

	auto Now() -> uint64
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			FClock::now().time_since_epoch()).count();
	}

	// Owns scheduler and compilation cleanup even after a fatal test assertion.
	class FQualificationLifetime
	{
	public:
		~FQualificationLifetime()
		{
			GCountAllocations.store(false);
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
			ShutdownAssetCompilingManager();
			ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		}
	};

	// Completion callbacks borrow batch counters, so drain before those counters
	// leave scope even when an admission or wait assertion aborts the batch.
	class FTextureBatchDrain
	{
	public:
		~FTextureBatchDrain()
		{
			GCountAllocations.store(false);
			FAssetCompilingManager::Get().FinishAllCompilation();
		}
	};

	struct FMeasurements
	{
		std::vector<uint64> QueueNanoseconds;
		std::vector<uint64> BatchNanoseconds;
		std::vector<uint64> Allocations;
		std::vector<uint64> AllocatedBytes;
		uint64 RetainedResultBytes = 0;
		uint64 PeakLiveAllocationBytes = 0;
		uint64 PeakDeclaredInFlightBytes = 0;

		auto Reserve() -> void
		{
			QueueNanoseconds.reserve(MeasuredBatches * BatchSize);
			BatchNanoseconds.reserve(MeasuredBatches);
			Allocations.reserve(MeasuredBatches);
			AllocatedBytes.reserve(MeasuredBatches);
		}

		auto Print(const char* Pilot) -> void
		{
			auto Percentile = [](std::vector<uint64> Values, uint32 Percent) {
				std::ranges::sort(Values);
				return Values[(Values.size() * Percent + 99) / 100 - 1];
			};
			std::cout << "ASYNC_PILOT {\"pilot\":\"" << Pilot
				<< "\",\"batches\":" << MeasuredBatches << ",\"batch_size\":" << BatchSize
				<< ",\"queue_median_ns\":" << Percentile(QueueNanoseconds, 50)
				<< ",\"queue_p95_ns\":" << Percentile(QueueNanoseconds, 95)
				<< ",\"batch_median_ns\":" << Percentile(BatchNanoseconds, 50)
				<< ",\"batch_p95_ns\":" << Percentile(BatchNanoseconds, 95)
				<< ",\"operations_per_second\":"
				<< (1e9 * BatchSize / Percentile(BatchNanoseconds, 50))
				<< ",\"allocations_median\":" << Percentile(Allocations, 50)
				<< ",\"allocations_p95\":" << Percentile(Allocations, 95)
				<< ",\"allocated_bytes_median\":" << Percentile(AllocatedBytes, 50)
				<< ",\"peak_live_allocation_bytes\":" << PeakLiveAllocationBytes
				<< ",\"retained_result_bytes\":" << RetainedResultBytes
				<< ",\"peak_declared_inflight_bytes\":" << PeakDeclaredInFlightBytes << "}\n";
		}
	};

	// Uses real warm-cache file I/O behind the production request/transform API.
	// Backend timestamps isolate root queue latency without changing Core logging.
	class FQualificationFileResource final : public FPackageResource
	{
	public:
		explicit FQualificationFileResource(std::filesystem::path InPath)
			: FPackageResource(RangeBytes * BatchSize), Path(std::move(InPath)) {}
		std::array<uint64, BatchSize> Started{};

	private:
		auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool& Cancelled)
			-> FPackageResourceReadResult override
		{
			Started[Offset / RangeBytes] = Now();
			if (Cancelled.load()) return {.Status = EPackageResourceReadStatus::Cancelled};
			std::ifstream Stream(Path, std::ios::binary);
			Stream.seekg(static_cast<std::streamoff>(Offset));
			FByteBuffer Bytes(Size);
			Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size));
			if (!Stream || Cancelled.load()) return {.Status = EPackageResourceReadStatus::IoError};
			return {.Status = EPackageResourceReadStatus::Success,
				.Buffer = FSharedByteBuffer::Take(std::move(Bytes))};
		}
		std::filesystem::path Path;
	};

	auto StartMeasurement() -> uint64
	{
		const uint64 Epoch = GAllocationEpoch.fetch_add(1) + 1;
		require(Epoch < GAllocationCounters.size());
		GCountAllocations.store(true);
		return Now();
	}

	auto FinishMeasurement(FMeasurements& Out, uint64 Start, bool Record) -> void
	{
		const uint64 End = Now();
		GCountAllocations.store(false);
		if (Record)
		{
			Out.BatchNanoseconds.push_back(End - Start);
			Out.Allocations.push_back(GAllocationCounters[GAllocationEpoch.load()].Count.load());
			Out.AllocatedBytes.push_back(GAllocationCounters[GAllocationEpoch.load()].AllocatedBytes.load());
			Out.PeakLiveAllocationBytes = std::max(Out.PeakLiveAllocationBytes, GAllocationCounters[GAllocationEpoch.load()].PeakLiveBytes.load());
		}
	}

	TEST(FAsyncTaskPilotQualificationTests, MeasuresPackageTransformAndTextureCommit)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		FQualificationLifetime Lifetime;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeAssetCompilingManager());
		FModuleManager::Get().LoadModuleChecked("TextureBuild");

		(void)StartMeasurement();
		void* CounterProbe = ::operator new(129);
		GCountAllocations.store(false);
		const auto& ProbeCounters = GAllocationCounters[GAllocationEpoch.load()];
		EXPECT_EQ(ProbeCounters.Count.load(), 1u);
		EXPECT_EQ(ProbeCounters.LiveBytes.load(), 129u);
		EXPECT_EQ(ProbeCounters.PeakLiveBytes.load(), 129u);
		::operator delete(CounterProbe);
		EXPECT_EQ(ProbeCounters.LiveBytes.load(), 0u);

		// Also verifies that the replacement observes an allocation inside Engine,
		// not only vector allocations emitted by this test translation unit.
		(void)StartMeasurement();
		auto AllocationProbe = FPackageResourceRequest::Completed({});
		GCountAllocations.store(false);
		ASSERT_GT(GAllocationCounters[GAllocationEpoch.load()].Count.load(), 0u);

		const auto Path = Testing::GetTestWorkDirectory() / "async-pilot.bin";
		{
			std::ofstream Stream(Path, std::ios::binary);
			const FByteBuffer Bytes(RangeBytes * BatchSize, std::byte{0x35});
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			ASSERT_TRUE(Stream.good());
		}
		FMeasurements Package;
		Package.Reserve();
		for (uint32 Batch = 0; Batch < WarmupBatches + MeasuredBatches; ++Batch)
		{
			auto Resource = std::make_shared<FQualificationFileResource>(Path);
			std::array<FPackageResourceRequest, BatchSize> Requests;
			std::array<FPackageResourceReadResult, BatchSize> Results;
			std::array<uint64, BatchSize> Submitted{};
			const uint64 Start = StartMeasurement();
			for (uint32 Index = 0; Index < BatchSize; ++Index)
			{
				Submitted[Index] = Now();
				Requests[Index] = FPackageResourceRequest::Transform(
					Resource->ReadRangeAsync(Index * RangeBytes, RangeBytes),
					[](FPackageResourceReadResult Input) {
						if (!Input) return Input;
						FByteBuffer Bytes(Input.Buffer.GetBytes().begin(), Input.Buffer.GetBytes().end());
						for (auto& Byte : Bytes) Byte ^= std::byte{0x7f};
						return FPackageResourceReadResult{.Status = EPackageResourceReadStatus::Success,
							.Buffer = FSharedByteBuffer::Take(std::move(Bytes))};
					});
			}
			for (uint32 Index = 0; Index < BatchSize; ++Index) Results[Index] = Requests[Index].Wait();
			// Wait() can publish before task-body return in the legacy API. Retire
			// all reads and reach scheduler quiescence before the next measured batch.
			FinishMeasurement(Package, Start, Batch >= WarmupBatches);
			for (uint32 Index = 0; Index < BatchSize; ++Index)
			{
				ASSERT_TRUE(Results[Index]);
				ASSERT_EQ(Results[Index].Buffer.GetSize(), RangeBytes);
				EXPECT_EQ(Results[Index].Buffer.GetBytes().front(), std::byte{0x4a});
				if (Batch >= WarmupBatches) Package.QueueNanoseconds.push_back(Resource->Started[Index] - Submitted[Index]);
			}
			Package.RetainedResultBytes = RangeBytes * BatchSize;
			Resource->Retire();
			const auto Deadline = FClock::now() + std::chrono::seconds(5);
			while (GetTaskSchedulerDiagnostics().NonterminalTaskCount && FClock::now() < Deadline)
				std::this_thread::yield();
			ASSERT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, 0u);
		}
		Package.Print("package_64k_read_xor");

		std::array<DTexture2D*, BatchSize> Textures{};
		Testing::RegisterMountPointForTests("/AsyncPilot/",
			(Testing::GetTestWorkDirectory() / "Content").generic_string() + "/");
		for (uint32 Index = 0; Index < BatchSize; ++Index)
		{
			FPackagePath PackagePath;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/Texture" + std::to_string(Index), PackagePath));
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Textures[Index]));
			ASSERT_NE(Textures[Index], nullptr);
		}
		FMeasurements Texture;
		Texture.Reserve();
		for (uint32 Batch = 0; Batch < WarmupBatches + MeasuredBatches; ++Batch)
		{
			std::array<FTexture2DCompilationRequest, BatchSize> Requests;
			for (auto& Request : Requests)
			{
				FTextureSourceData Source;
				Source.Width = 64;
				Source.Height = 64;
				Source.SourceChannelCount = 4;
				Source.Format = ETextureSourceFormat::RGBA8;
				Source.Pixels.resize(64 * 64 * 4, static_cast<std::byte>(Batch + 1));
				Request.Build.ImportedData = FTexture2DImportedData(Source);
				Request.Build.bPersistDerivedData = false;
			}
			uint32 Completions = 0;
			bool Success = true;
			std::string Error;
			FTextureBatchDrain Drain;
			const uint64 Start = StartMeasurement();
			for (uint32 Index = 0; Index < BatchSize; ++Index)
			{
				ASSERT_TRUE(SubmitTexture2DCompilation(*Textures[Index], std::move(Requests[Index]), Error,
					[&](FTexture2DCompilationResult Result) { ++Completions; Success &= Result.Succeeded(); })) << Error;
			}
			Texture.PeakDeclaredInFlightBytes = std::max(Texture.PeakDeclaredInFlightBytes,
				GetTexture2DCompilationManagerDiagnostics().InFlightEstimatedBytes);
			for (auto* Object : Textures) ASSERT_TRUE(WaitForTexture2DCompilation(*Object, 10.0));
			FinishMeasurement(Texture, Start, Batch >= WarmupBatches);
			ASSERT_EQ(Completions, BatchSize);
			ASSERT_TRUE(Success);
			uint64 ResultBytes = 0;
			for (auto* Object : Textures)
			{
				const auto Diagnostic = GetTexture2DCompilationDiagnostic(*Object);
				if (Batch >= WarmupBatches) Texture.QueueNanoseconds.push_back(Diagnostic.QueuedNanoseconds);
				ResultBytes += Diagnostic.Metrics.ResultBytes;
				ASSERT_NE(Object->GetPlatformData(), nullptr);
			}
			Texture.RetainedResultBytes = std::max(Texture.RetainedResultBytes, ResultBytes);
			ASSERT_EQ(GetTexture2DCompilationManagerDiagnostics().ActiveRecordCount, 0u);
		}
		Texture.Print("texture_64_rgba8_commit");
	}
}
