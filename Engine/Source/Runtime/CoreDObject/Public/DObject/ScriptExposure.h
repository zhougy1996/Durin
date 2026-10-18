#pragma once
#include "CoreMinimal.h"

namespace Durin
{
    // Copied at native registration; independent of any scripting interpreter.
    enum class EScriptExposure : uint8 { None = 0, Type = 1, Callable = 2, ReadOnly = 4, ReadWrite = 8, NoExport = 16 };
    ENUM_CLASS_FLAGS(EScriptExposure);
    struct FScriptExposure
    {
        EScriptExposure Flags = EScriptExposure::None;
        std::string Name;
        std::string Module;
        auto Has(EScriptExposure Flag) const -> bool { return EnumHasAnyFlags(Flags, Flag); }
        auto Validate() const -> bool
        {
            if (Has(EScriptExposure::ReadOnly) && Has(EScriptExposure::ReadWrite)) return false;
            if (Name.empty()) return true;
            for (size_t Index = 0; Index < Name.size(); ++Index)
            {
                const auto C = Name[Index];
                if (!(C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z')
                    || (Index && C >= '0' && C <= '9'))) return false;
            }
            return true;
        }
        auto operator==(const FScriptExposure&) const -> bool = default;
    };
}
