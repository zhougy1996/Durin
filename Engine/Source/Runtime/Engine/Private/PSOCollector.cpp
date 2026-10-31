#include "PSOPrecacheMaterial.h"

namespace Durin
{
    namespace
    {
        struct FCollectorEntry { std::string Name; PSOCollectorCreateFunction Create = nullptr; };
        struct FCollectorRegistry
        {
            std::mutex Mutex;
            std::array<std::array<FCollectorEntry, FPSOCollectorCreateManager::MaxPSOCollectorCount>, static_cast<size_t>(EShadingPath::Num)> Entries;
            std::array<int32, static_cast<size_t>(EShadingPath::Num)> Counts{};
        };
        auto Registry() -> FCollectorRegistry& { static FCollectorRegistry Value; return Value; }
        auto ValidPath(EShadingPath Path) -> bool { return Path < EShadingPath::Num; }
        auto ValidIndex(int32 Index) -> bool { return Index >= 0 && Index < FPSOCollectorCreateManager::MaxPSOCollectorCount; }
    }
    FRegisterPSOCollectorCreateFunction::FRegisterPSOCollectorCreateFunction(PSOCollectorCreateFunction InCreate, EShadingPath InPath, std::string_view Name)
        : Create(InCreate), ShadingPath(InPath)
    {
        if (!Create || !ValidPath(ShadingPath) || Name.empty()) return;
        auto& R = Registry(); std::lock_guard Lock(R.Mutex);
        auto& Entries = R.Entries[static_cast<size_t>(ShadingPath)]; auto& Count = R.Counts[static_cast<size_t>(ShadingPath)];
        for (const auto& Entry : Entries) if (Entry.Create && Entry.Name == Name) return;
        for (int32 Slot = 0; Slot < Count; ++Slot) if (!Entries[Slot].Create) { Index = Slot; break; }
        if (Index < 0) { if (Count == FPSOCollectorCreateManager::MaxPSOCollectorCount) return; Index = Count++; }
        Entries[Index] = {std::string(Name), Create};
    }
    FRegisterPSOCollectorCreateFunction::~FRegisterPSOCollectorCreateFunction()
    {
        if (Index < 0) return;
        auto& R = Registry(); std::lock_guard Lock(R.Mutex);
        auto& Entries = R.Entries[static_cast<size_t>(ShadingPath)]; auto& Count = R.Counts[static_cast<size_t>(ShadingPath)];
        require(Entries[Index].Create == Create); Entries[Index] = {};
        while (Count && !Entries[Count - 1].Create) --Count;
    }
    auto FPSOCollectorCreateManager::GetCreateFunction(EShadingPath Path, int32 Index) -> PSOCollectorCreateFunction
    {
        if (!ValidPath(Path) || !ValidIndex(Index)) return nullptr;
        auto& R = Registry(); std::lock_guard Lock(R.Mutex); return R.Entries[static_cast<size_t>(Path)][Index].Create;
    }
    auto FPSOCollectorCreateManager::GetIndex(EShadingPath Path, std::string_view Name) -> int32
    {
        if (!ValidPath(Path)) return -1;
        auto& R = Registry(); std::lock_guard Lock(R.Mutex); const auto& Entries = R.Entries[static_cast<size_t>(Path)];
        for (int32 Index = 0; Index < R.Counts[static_cast<size_t>(Path)]; ++Index)
            if (Entries[Index].Create && Entries[Index].Name == Name) return Index;
        return -1;
    }
    auto FPSOCollectorCreateManager::GetName(EShadingPath Path, int32 Index) -> std::string_view
    {
        if (!ValidPath(Path) || !ValidIndex(Index)) return {};
        auto& R = Registry(); std::lock_guard Lock(R.Mutex); return R.Entries[static_cast<size_t>(Path)][Index].Name;
    }
    auto FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath Path) -> int32
    {
        if (!ValidPath(Path)) return 0;
        auto& R = Registry(); std::lock_guard Lock(R.Mutex); return R.Counts[static_cast<size_t>(Path)];
    }
}
