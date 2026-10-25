#include "CoreMinimal.h"
#include <gtest/gtest.h>
#include "RendererResourceSlotCache.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	namespace
	{
		using FResult = TRenderResourceCreateResult<int>;
		using FCache = TRendererResourceSlotCache<int, int>;

		TEST(FRendererResourceSlotCacheQualificationTests, LookupAndRetentionBaseline)
		{
			const auto Root = Testing::CreateTestFixtureDirectory("RendererSlotBaseline");
			std::string Csv = "working_set,scenario,run,sample,nanoseconds,recreations,evictions,retained_payload_bytes,entries,accounted_slot_bytes\n";
			for (const int WorkingSet : {256, 512, 1024, 4096})
			for (const bool Hot : {false, true})
			for (int Run = 0; Run < 3; ++Run)
			{
				FCache Cache;
				FRenderResourceGeneration Generation;
				for (int Sample = -5; Sample < 30; ++Sample)
				{
					size_t Recreations = 0, Evictions = 0;
					const auto Start = std::chrono::steady_clock::now();
					for (int Access = 0; Access < WorkingSet; ++Access)
					{
						const int Key = Hot && Access % 10 != 0 ? Access % 64
							: Hot ? 64 + (Access / 10 + (Sample + 5) * ((WorkingSet + 9) / 10)) % (WorkingSet - 64) : Access;
						const size_t Before = Cache.Num();
						auto& Entry = Cache.FindOrAddBounded(Key, 512);
						const bool Missing = Entry.Slot.GetPayload() == nullptr;
						if (Missing && Before == 512) ++Evictions;
						Entry.Slot.Resolve(Generation, [&] {
							++Recreations;
							return FResult::Success(Key);
						}, [](const FRenderResourceCreateDiagnostic&) {});
					}
					const auto Ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - Start).count();
					const auto SlotBytes = []<typename T>(const T& Value) -> size_t {
						if constexpr (requires { Value.GetAccountedSlotBytes(); }) return Value.GetAccountedSlotBytes();
						else return Value.Num() * sizeof(typename T::FEntry);
					}(Cache);
					if (Sample >= 0) Csv += std::format("{},{},{},{},{},{},{},{},{},{}\n", WorkingSet,
						Hot ? "hot" : "cyclic", Run, Sample, Ns, Recreations, Evictions,
						Cache.GetRetainedPayloadWeight([](int, int) { return sizeof(int); }), Cache.Num(), SlotBytes);
				}
			}
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Csv)), Root / "samples.csv"));
			std::cout << "[RendererSlotBaseline] csv=" << (Root / "samples.csv").generic_string() << '\n';
		}

	}
}
