#pragma once

#include "Logging/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <thread>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace Durin::Testing
{
	// Qualification executables own this logger lifecycle. Suppress trace/debug
	// sink I/O while retaining the normal bounded in-process diagnostic history.
	// Startup, flush and shutdown stay outside all measured intervals.
	class FQualificationLogSession
	{
	public:
		explicit FQualificationLogSession(std::string Directory)
		{
			bStarted = FLogger::Get().Initialize({.ConsoleLevel = ELogLevel::Warn,
				.FileLevel = ELogLevel::Warn, .HistoryCapacity = 256,
				.LogDirectory = std::move(Directory), .RuntimeVariant = "BuildQualification"});
		}
		~FQualificationLogSession() { Stop(); }
		FQualificationLogSession(const FQualificationLogSession&) = delete;
		auto operator=(const FQualificationLogSession&) -> FQualificationLogSession& = delete;
		auto IsStarted() const -> bool { return bStarted; }
		auto Stop() -> void
		{
			if (bStarted) { FLogger::Get().Shutdown(); bStarted = false; }
		}
	private:
		bool bStarted = false;
	};

	struct FQualificationAllocationSample
	{
		bool bAvailable = false;
		uint64 BaselineBytes = 0;
		uint64 PeakBytes = 0;
		uint64 SampleCount = 0;
		auto GetPeakIncrease() const -> uint64 { return PeakBytes - BaselineBytes; }
	};

	// Samples the macOS default malloc zone, not exact request allocations or RSS.
	// This includes other threads in this process, excludes non-zone allocations,
	// and may miss allocations living for less than one millisecond. Report the
	// metric with these limitations; never use it as an exact admission estimate.
	class FQualificationAllocationSampler
	{
	public:
		FQualificationAllocationSampler()
		{
#if defined(__APPLE__)
			Sampler = std::jthread([this](std::stop_token Stop) {
				Ready.count_down();
				Armed.wait(false);
				while (!Stop.stop_requested())
				{
					Sample();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
			Ready.wait();
			// Exclude the sampler's own startup allocation from the increase.
			Result.bAvailable = true;
			Result.BaselineBytes = ReadBytes();
			Peak.store(Result.BaselineBytes);
			Armed.store(true);
			Armed.notify_one();
#endif
		}
		~FQualificationAllocationSampler() { Finish(); }
		FQualificationAllocationSampler(const FQualificationAllocationSampler&) = delete;
		auto operator=(const FQualificationAllocationSampler&) -> FQualificationAllocationSampler& = delete;

		auto Finish() -> FQualificationAllocationSample
		{
			if (Sampler.joinable())
			{
				Sample();
				Sampler.request_stop();
				Sampler.join();
				Result.PeakBytes = Peak.load();
				Result.SampleCount = Samples.load();
			}
			return Result;
		}
	private:
		static auto ReadBytes() -> uint64
		{
#if defined(__APPLE__)
			malloc_statistics_t Stats{};
			malloc_zone_statistics(malloc_default_zone(), &Stats);
			return Stats.size_in_use;
#else
			return 0;
#endif
		}
		auto Sample() -> void
		{
			const auto Bytes = ReadBytes();
			auto Previous = Peak.load(std::memory_order_relaxed);
			while (Previous < Bytes && !Peak.compare_exchange_weak(Previous, Bytes, std::memory_order_relaxed)) {}
			Samples.fetch_add(1, std::memory_order_relaxed);
		}
		FQualificationAllocationSample Result;
		std::atomic<uint64> Peak{0}, Samples{0};
		std::atomic_bool Armed{false};
		std::latch Ready{1};
		std::jthread Sampler;
	};
}
