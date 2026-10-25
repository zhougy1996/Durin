#pragma once

#include "CoreMinimal.h"
#include "RHICompletion.h"

namespace Durin
{
    // Logical renderer invalidation never authorizes release of a physical-device lease.
    class FRendererResourceRetirementQueue
    {
    public:
        auto Retire(std::shared_ptr<const void> Owner, FRHIGPUSyncPointRef Completion,
            uint64 RHIDeviceGeneration) -> void
        {
            if (Owner) Entries.push_back({std::move(Owner), std::move(Completion), RHIDeviceGeneration});
        }
        auto Collect(const FRHIQueueCapabilities& PhysicalDevice) -> void
        {
            std::erase_if(Entries, [&](const auto& Entry) {
                return Entry.Completion.IsRetirementEligible()
                    || (PhysicalDevice.DeviceGeneration != 0
                        && Entry.RHIDeviceGeneration != PhysicalDevice.DeviceGeneration);
            });
        }
        // Only shutdown may release failed/canceled leases after the host drains the GPU.
        template<class DrainDevice>
        auto Drain(DrainDevice&& Drain) -> void
        {
            if (Entries.empty()) return;
            std::forward<DrainDevice>(Drain)();
            Entries.clear();
        }
        ~FRendererResourceRetirementQueue() { require(Entries.empty()); }
    private:
        struct FEntry
        {
            std::shared_ptr<const void> Owner;
            FRHIGPUSyncPointRef Completion;
            uint64 RHIDeviceGeneration;
        };
        std::vector<FEntry> Entries;
    };
}
