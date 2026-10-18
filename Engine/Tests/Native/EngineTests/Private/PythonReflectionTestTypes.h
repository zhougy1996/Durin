#pragma once
#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "PythonReflectionTestTypes.gen.h"

namespace Durin::PythonFixtures
{
    DSTRUCT()
    struct FSignatureDependency
    {
        GENERATED_BODY()
        DPROPERTY()
        int32 Value = 7;
    };
    DENUM(ScriptType)
    enum class EReflectionMode : uint8 { Idle, Ready };
    DCLASS(ScriptType, ScriptNoExport, NoClassDefaultObject)
    class DHiddenType : public DObject
    {
        GENERATED_BODY()
    public:
        explicit DHiddenType(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(Abstract, NoClassDefaultObject)
    class DReflectionBase : public DEditorSubsystem
    {
        GENERATED_BODY()
    public:
        explicit DReflectionBase(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class DReflectionService : public DReflectionBase
    {
        GENERATED_BODY()
    public:
        explicit DReflectionService(const FObjectInitializer& Initializer) : Super(Initializer) {}
        DFUNCTION(ScriptCallable)
        auto Describe() const -> FSignatureDependency { return {}; }
        DFUNCTION(ScriptCallable)
        auto HiddenDependency() const -> DHiddenType* { return nullptr; }
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class DSecondaryService : public DEditorSubsystem
    {
        GENERATED_BODY()
    public:
        explicit DSecondaryService(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptType, ScriptName = "SharedAlias", NoClassDefaultObject)
    class DFirstAlias : public DObject
    {
        GENERATED_BODY()
    public:
        explicit DFirstAlias(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptType, ScriptName = "SharedAlias", NoClassDefaultObject)
    class DSecondAlias : public DObject
    {
        GENERATED_BODY()
    public:
        explicit DSecondAlias(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };

}
