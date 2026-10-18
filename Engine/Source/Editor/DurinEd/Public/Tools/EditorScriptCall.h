#pragma once
#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/ObjectKey.h"
#include "DObject/ObjectLifecycle.h"
#include "Tools/Tool.h"

namespace Durin
{
    class DEditorEngine;
    class DLevel;
    class FSubsystemWorkGate;
    enum class EEditorScriptEditPolicy : uint8 { DocumentManaged, NonDocumentReadOnly, NonDocumentReadWrite };
    enum class EEditorScriptTargetAccess : uint8 { Writable, ReadOnly, Unavailable };
    struct FEditorScriptCallError { std::string Message; };
    struct FEditorScriptCallContext
    {
        DEditorEngine* Host = nullptr;
        std::shared_ptr<const FSubsystemWorkGate> Gate;
        EEditorScriptEditPolicy EditPolicy = EEditorScriptEditPolicy::DocumentManaged;
        FToolCancellationToken Cancellation;
        FToolClientContext Client;
        EToolSource Source = EToolSource::External;
    };
    // Synchronous native admission seam. No interpreter or job object escapes here.
    class FScopedEditorScriptCall final
    {
    public:
        DURINED_API explicit FScopedEditorScriptCall(FEditorScriptCallContext Context);
        DURINED_API ~FScopedEditorScriptCall();
        FScopedEditorScriptCall(const FScopedEditorScriptCall&) = delete;
        auto operator=(const FScopedEditorScriptCall&) -> FScopedEditorScriptCall& = delete;
        DURINED_API static auto Current() -> FScopedEditorScriptCall*;
        DURINED_API auto IsAdmitted() const -> bool;
        DURINED_API auto ResolveEditAccess(DObject& Target) -> EEditorScriptTargetAccess;
        auto GetContext() const -> const FEditorScriptCallContext& { return Context; }
        auto GetError() const -> const std::optional<FEditorScriptCallError>& { return Error; }
        auto Reject(std::string Message) -> void { if (!Error) Error = FEditorScriptCallError{std::move(Message)}; }
    private:
        FGarbageCollectionDeferralScope Deferral;
        FEditorScriptCallContext Context;
        FScopedEditorScriptCall* Previous;
        std::optional<FEditorScriptCallError> Error;
    };
    // The document owner reports its live state, and retires this token before its model.
    class FEditorScriptTargetPolicyRegistration final
    {
    public:
        DURINED_API FEditorScriptTargetPolicyRegistration(DEditorEngine& Host, DLevel& Level,
            std::function<EEditorScriptTargetAccess()> Resolve);
        DURINED_API ~FEditorScriptTargetPolicyRegistration();
        FEditorScriptTargetPolicyRegistration(const FEditorScriptTargetPolicyRegistration&) = delete;
        auto operator=(const FEditorScriptTargetPolicyRegistration&) -> FEditorScriptTargetPolicyRegistration& = delete;
    private:
        uint64 Identity;
    };
}
