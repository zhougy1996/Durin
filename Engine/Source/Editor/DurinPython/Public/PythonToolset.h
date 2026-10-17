#pragma once

#include "CoreMinimal.h"
#include "DurinPythonAPI.h"
#include "Tools/NativeToolset.h"

namespace Durin
{
    class FPythonRuntime;
    // Owner retains the definition and feature token; runtime lifecycle remains independent.
    DURINPYTHON_API auto CreatePythonToolset(std::shared_ptr<FPythonRuntime> Runtime) -> std::unique_ptr<FNativeToolset>;
}
