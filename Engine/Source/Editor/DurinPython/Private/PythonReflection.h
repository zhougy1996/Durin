#pragma once
#include "CoreMinimal.h"
#include "Tools/EditorScriptCall.h"

// Opaque CPython objects keep this private header independent of Python include order.
struct _object;
namespace Durin::PythonReflection
{
    auto Start(_object* Module, _object* AccessError,
        std::function<std::optional<FEditorScriptCallContext>()> GetContext) -> bool;
    auto Stop() -> void;
    auto Publish() -> bool;
    auto GetType(std::string_view QualifiedName) -> _object*;
    auto GetEditorSubsystem(_object* Type) -> _object*;
    auto GenerateStubs() -> _object*;
    auto PurgeGate(const FSubsystemWorkGate* Gate) -> void;
}
