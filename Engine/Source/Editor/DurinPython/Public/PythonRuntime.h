#pragma once
#include "CoreMinimal.h"
#include "DurinPythonAPI.h"
#include "Tools/Tool.h"

namespace Durin
{
    class FEditorToolService;
    // Script jobs belong to one runtime/session; terminal snapshots own their data.
    enum class EPythonJobState : uint8 { Queued, Running, Succeeded, Failed, Cancelled };
    struct FPythonJobResult
    {
        EPythonJobState State = EPythonJobState::Queued;
        std::string Output;
        std::string Error;
    };

    // Optional runtime directory supports deployment verification and native hosts.
    struct FPythonRuntimeOptions
    {
        std::filesystem::path RuntimeDirectory;
        // Total elapsed time from admission, including queueing and suspension.
        std::optional<std::chrono::milliseconds> JobDeadline;
    };

    // One interpreter owned by GameThread. Jobs execute at the editor script
    // pump, outside tool provider dispatch. Submission/query/cancel are detached.
    // Start/Stop and destruction belong to GameThread. Stop during a callback
    // signals cancellation; finalization waits for script/native return.
    class FPythonRuntime final
    {
    public:
        DURINPYTHON_API FPythonRuntime();
        DURINPYTHON_API ~FPythonRuntime();
        FPythonRuntime(const FPythonRuntime&) = delete;
        auto operator=(const FPythonRuntime&) -> FPythonRuntime& = delete;
        DURINPYTHON_API auto Start(const FPythonRuntimeOptions& Options = {}) -> std::expected<void, std::string>;
        DURINPYTHON_API auto Stop() noexcept -> void;
        // Acquire the endpoint from the explicitly selected editor on GameThread.
        DURINPYTHON_API auto CreateSession(std::shared_ptr<FEditorToolService> Endpoint,
            EToolSource Source = EToolSource::External, const FToolClientContext& Client = {}) -> std::expected<uint64, std::string>;
        DURINPYTHON_API auto CloseSession(uint64 Session) -> void;
        DURINPYTHON_API auto RunString(uint64 Session, std::string Code,
            std::string Filename = "<durin>") -> std::expected<uint64, std::string>;
        // Relative paths use the project directory; contents are captured before queueing.
        DURINPYTHON_API auto RunFile(uint64 Session, const std::filesystem::path& Path) -> std::expected<uint64, std::string>;
        DURINPYTHON_API auto GetJobResult(uint64 Job) const -> std::expected<FPythonJobResult, std::string>;
        DURINPYTHON_API auto CancelJob(uint64 Job) -> bool;
    private:
        auto StopLocked() noexcept -> void;
        struct FImpl;
        mutable std::shared_mutex LifecycleMutex;
        std::unique_ptr<FImpl> Impl;
    };
}
