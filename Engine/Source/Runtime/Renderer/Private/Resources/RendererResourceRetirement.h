#pragma once

#include "CoreMinimal.h"
#include "RHICommandList.h"
#include "RHICompletion.h"

namespace Durin
{
    // Logical renderer invalidation never authorizes release of a physical-device lease.
    class FRendererResourceRetirementQueue
    {
    public:
        auto Retire(std::shared_ptr<const void> Owner, FRHIGPUSyncPointRef Completion) -> void
        {
            if (Owner) Entries.push_back({std::move(Owner), std::move(Completion)});
        }
        auto Collect() -> void
        {
            std::erase_if(Entries, [](const auto& Entry) {
                return Entry.Completion.IsRetirementEligible();
            });
        }
        // Shutdown drains the GPU before releasing entries still held by the queue.
        auto Shutdown(FRHICommandListImmediate& Commands) -> void
        {
            if (Entries.empty()) return;
            Commands.BlockUntilGPUIdle();
            Entries.clear();
        }
        ~FRendererResourceRetirementQueue() { require(Entries.empty()); }
    private:
        struct FEntry
        {
            std::shared_ptr<const void> Owner;
            FRHIGPUSyncPointRef Completion;
        };
        std::vector<FEntry> Entries;
    };
}
