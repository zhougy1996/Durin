#pragma once
#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/Class.h"
#include "DObject/ObjectPtr.h"
#include "DObject/SoftObjectPtr.h"
#include "Tools/EditorScriptCall.h"
#include "Math/Transform.h"
#include "Misc/Guid.h"
#include "Engine/Actor.h"
#include "DObject/PropertyChange.h"
#include "DObject/Property.h"
#include "PythonReflectionTestTypes.gen.h"

namespace Durin::PythonFixtures
{
    DSTRUCT()
    struct FSignatureDependency
    {
        GENERATED_BODY()
        DPROPERTY(ScriptReadOnly)
        int32 Value = 7;
    };
    DSTRUCT(ScriptType)
    struct FReflectionValue
    {
        GENERATED_BODY()
        inline static int32 Live = 0;
        FReflectionValue() { ++Live; }
        FReflectionValue(const FReflectionValue& Other) : Text(Other.Text), Numbers(Other.Numbers), Target(Other.Target), Pose(Other.Pose)
        { std::copy(std::begin(Other.Fixed), std::end(Other.Fixed), Fixed); ++Live; }
        auto operator=(const FReflectionValue&) -> FReflectionValue& = default;
        ~FReflectionValue() { --Live; }
        DPROPERTY(ScriptReadWrite)
        std::string Text = "initial";
        DPROPERTY(ScriptReadWrite)
        std::vector<int32> Numbers;
        DPROPERTY(ScriptReadWrite)
        TObjectPtr<DObject> Target;
        DPROPERTY(ScriptReadWrite)
        FTransform Pose;
        DPROPERTY(ScriptReadWrite)
        int32 Fixed[3] = {1, 2, 3};
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class AReflectionActor : public AActor
    {
        GENERATED_BODY()
    public:
        explicit AReflectionActor(const FObjectInitializer& Initializer) : Super(Initializer) { SetFlags(EObjectFlags::Transactional); }
        DPROPERTY(Edit, ScriptReadWrite)
        int32 Value = 1;
        DPROPERTY(Edit, ScriptReadOnly)
        int32 HelperOnly = 2;
        DPROPERTY(Edit, ReadOnly)
        int32 Locked = 3;
        DPROPERTY(Edit, NonTransactional)
        int32 Unrecorded = 4;
        DPROPERTY(Edit, ScriptNoExport)
        int32 Secret = 5;
        DPROPERTY(Edit)
        FReflectionValue Data;
        DPROPERTY(Edit)
        std::vector<int32> Numbers;
        DPROPERTY(Edit)
        std::unordered_map<FName, int32> Mapping;
        DPROPERTY(Edit)
        int32 Fixed[3] = {1, 2, 3};
        DPROPERTY(Edit, ScriptReadWrite)
        bool bEnabled = false;
        std::vector<EPropertyChangePhase> Changes;
        uint32 Validations = 0;
        std::function<void()> BeforePublication;
        auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override
        {
            ++Validations;
            if (BeforePublication) BeforePublication();
            if (Proposal.MemberProperty->NamePrivate.ToString() == "Value")
            {
                auto& Candidate = *static_cast<int32*>(Proposal.DraftRootProperty->GetValuePtr(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
                if (Candidate < 0) return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::Rejected);
                Candidate = std::min(Candidate, 100);
            }
            return Super::PreEditChangeProperty(Proposal);
        }
        auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override
        { Changes.push_back(Event.Phase); Super::PostEditChangeProperty(Event); }
    };
    // Exercises native preparation callbacks through the production script adapter.
    DCLASS(ScriptType, NoClassDefaultObject)
    class AReflectionLifecycleActor : public AActor
    {
        GENERATED_BODY()
    public:
        explicit AReflectionLifecycleActor(const FObjectInitializer& Initializer) : Super(Initializer)
        { SetFlags(EObjectFlags::Transactional); SetRootComponent(CreateDefaultComponent<DSceneComponent>("Root")); }
        inline static std::function<void(AReflectionLifecycleActor&)> ConstructCallback;
        inline static std::function<void(AReflectionLifecycleActor&)> LoadCallback;
        auto PostLoad() -> void override
        { Super::PostLoad(); if (const auto Callback = LoadCallback) Callback(*this); }
    protected:
        auto OnNativeConstruct(FActorConstructionContext& Context, std::string& Error) -> bool override
        { if (const auto Callback = ConstructCallback) Callback(*this); return Super::OnNativeConstruct(Context, Error); }
    };
    DENUM(ScriptType)
    enum class EReflectionMode : uint8 { Idle, Ready };
    DENUM(ScriptType)
    enum class EOtherMode : uint8 { Idle, Ready };
    DCLASS(ScriptType, ScriptNoExport, NoClassDefaultObject)
    class DHiddenType : public DObject
    {
        GENERATED_BODY()
    public:
        explicit DHiddenType(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class DScriptTypeBase : public DObject
    {
        GENERATED_BODY()
    public:
        explicit DScriptTypeBase(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(NoClassDefaultObject)
    class DInheritedScriptType : public DScriptTypeBase
    {
        GENERATED_BODY()
    public:
        explicit DInheritedScriptType(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(NoClassDefaultObject)
    class DInheritedScriptGrandchild : public DInheritedScriptType
    {
        GENERATED_BODY()
    public:
        explicit DInheritedScriptGrandchild(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptNoExport, NoClassDefaultObject)
    class DExcludedScriptType : public DScriptTypeBase
    {
        GENERATED_BODY()
    public:
        explicit DExcludedScriptType(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class DExcludedScriptGrandchild : public DExcludedScriptType
    {
        GENERATED_BODY()
    public:
        explicit DExcludedScriptGrandchild(const FObjectInitializer& Initializer) : Super(Initializer) {}
    };
    DCLASS(Abstract, NoClassDefaultObject)
    class DReflectionBase : public DEditorSubsystem
    {
        GENERATED_BODY()
    public:
        explicit DReflectionBase(const FObjectInitializer& Initializer) : Super(Initializer) {}
        DFUNCTION(ScriptCallable)
        virtual auto VirtualValue(int32 Value) const -> int32 { return Value + 1; }
        DFUNCTION(ScriptCallable)
        virtual auto MaskedValue() const -> int32 { return 1; }
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
        DFUNCTION(ScriptCallable)
        auto InheritedHiddenDependency() const -> DExcludedScriptGrandchild* { return nullptr; }
        DPROPERTY(ScriptReadOnly)
        int32 Calls = 0;
        DPROPERTY(Edit, ScriptReadWrite)
        int32 ServiceValue = 7;
        DPROPERTY()
        TObjectPtr<DObject> EditTarget;
        DFUNCTION(ScriptCallable)
        auto GetEditTarget() const -> DObject* { return EditTarget.Get(); }
        inline static int32 NativeDefault = 31;
        DFUNCTION(ScriptCallable)
        auto Add(int32 Value, int32 Amount = 5) -> int32 { ++Calls; return Value + Amount; }
        DFUNCTION(ScriptCallable)
        auto NativeOnly(int32 Value = NativeDefault) -> int32 { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto VirtualValue(int32 Value) const -> int32 override { return Value + 10; }
        DFUNCTION()
        auto MaskedValue() const -> int32 override { return 2; }
        DFUNCTION(ScriptCallable)
        auto EchoValue(const FReflectionValue& Value) -> FReflectionValue { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoObject(DObject* Value) -> DObject* { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoClass(DClass* Value) -> DClass* { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoMode(EReflectionMode Value) -> EReflectionMode { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoMap(const std::unordered_map<FName, std::vector<int32>>& Value) -> std::unordered_map<FName, std::vector<int32>> { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoUnsigned(uint64 Value) -> uint64 { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoSigned(int8 Value) -> int8 { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoFloat(float Value) -> float { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoName(FName Value) -> FName { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto EchoGuid(FGuid Value) -> FGuid { ++Calls; return Value; }
        DFUNCTION(ScriptCallable)
        auto Multi(std::string& Text, int32& Value) -> int32 { ++Calls; Text += "!"; ++Value; return 42; }
        DFUNCTION(ScriptCallable)
        auto TryValue(bool Success, int32& Value) -> bool { ++Calls; Value = 9; return Success; }
        DFUNCTION(ScriptCallable)
        auto Nothing() -> void { ++Calls; }
        DFUNCTION(ScriptCallable)
        auto OversizedOutput() -> std::string { ++Calls; return std::string(16 * 1024 * 1024, 'x'); }
        DFUNCTION(ScriptCallable)
        auto Reject() -> int32 { FScopedEditorScriptCall::Current()->Reject("fixture rejection"); return 0; }
        DFUNCTION(ScriptCallable)
        auto Throw(const FReflectionValue& Value) -> void { ++Calls; throw std::runtime_error("fixture failure"); }
        DFUNCTION(ScriptCallable)
        auto Unsupported(Durin::TSoftObjectPtr<Durin::DObject> Value) -> void { ++Calls; }
    };
    DCLASS(ScriptType, NoClassDefaultObject)
    class DSecondaryService : public DEditorSubsystem
    {
        GENERATED_BODY()
    public:
        explicit DSecondaryService(const FObjectInitializer& Initializer) : Super(Initializer) {}
        DFUNCTION(ScriptCallable)
        auto Join(const std::vector<std::string>& Values, const std::string& Suffix = "!") const -> std::string
        { std::string Result; for (const auto& V : Values) Result += V; return Result + Suffix; }
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
