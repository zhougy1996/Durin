#include "PythonRuntime.h"
#include "Tools/EditorToolService.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorScriptPump.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transactor.h"
#include "DObject/WeakObjectPtr.h"
#include "Threading/RunnableThread.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformLibrary.h"

#if DURIN_WITH_PYTHON
// The packaged interpreter uses the release ABI in every editor configuration.
#pragma push_macro("_DEBUG")
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#if PY_VERSION_HEX != 0x030d0ff0 || defined(Py_GIL_DISABLED)
#error Durin embedding requires the CPython 3.13.15 release GIL ABI
#endif
#pragma pop_macro("_DEBUG")
#include "PythonABI.h"
#include "PythonReflection.h"
#endif

namespace Durin
{
    namespace
    {
        constexpr size_t MaxSource = 1024 * 1024;
        constexpr size_t MaxOutput = 64 * 1024;
        auto Utf8Prefix(std::string_view Text, size_t Limit) -> size_t
        {
            size_t Count = std::min(Text.size(), Limit);
            if (Count < Text.size()) while (Count && (static_cast<unsigned char>(Text[Count]) & 0xc0) == 0x80) --Count;
            return Count;
        }
        std::atomic<uint64> NextIdentity{1};
        std::atomic<bool> InterpreterOwned{false};
        auto Terminal(EPythonJobState State) -> bool
        { return State != EPythonJobState::Queued && State != EPythonJobState::Running; }

#if DURIN_WITH_PYTHON
        struct FPyDelete { auto operator()(PyObject* Object) const -> void { Py_XDECREF(Object); } };
        using FPy = std::unique_ptr<PyObject, FPyDelete>;
#endif
        // Owned job data is independent of editor/provider objects.
        struct FJob
        {
            uint64 Id = 0, Session = 0;
            EToolSource Source = EToolSource::External;
            std::shared_ptr<FEditorToolService> Endpoint;
            FToolClientContext Client;
            FPythonSessionOptions Options;
            std::string Code, Filename;
            FToolCancellationSource Cancellation;
            std::shared_ptr<const FSubsystemWorkGate> Gate;
            std::mutex Mutex;
            FPythonJobResult Result;
            bool OutputOverflow = false;
            std::optional<std::chrono::steady_clock::time_point> Deadline;
            std::map<uint64, FEditorToolSubmission> Invocations;
            struct FScope
            {
                uint64 Id;
                std::unique_ptr<Editor::FScopedTransaction> Native;
                TWeakObjectPtr<DTransactor> Owner;
            };
            std::vector<FScope> Scopes;
#if DURIN_WITH_PYTHON
            struct FContinuation { FPy Object; uint64 Waiting = 0; };
            std::deque<FContinuation> Continuations;
            FPy Globals, Tools, Output;
            bool Started = false, Cleaning = false, InContinuation = false;
            uint64 LastPump = 0;
#endif
        };
    }

#if DURIN_WITH_PYTHON
    namespace
    {
        thread_local FJob* CurrentJob = nullptr;
        thread_local DEditorEngine* CurrentEditor = nullptr;
        PyObject* ToolError = nullptr;
        PyObject* CancelledError = nullptr;
        PyObject* NotReady = nullptr;
        PyObject* ScriptAccessError = nullptr;

        auto Fail(const char* Message) -> PyObject* { PyErr_SetString(PyExc_ValueError, Message); return nullptr; }
        auto CheckCancelled() -> bool
        {
            if (!CurrentJob) { PyErr_SetString(PyExc_RuntimeError, "No active Durin script job."); return true; }
            if (CurrentJob->Cancellation.GetToken().IsCancellationRequested() || CurrentJob->Client.Cancellation.IsCancellationRequested() || !CurrentJob->Gate->IsOpen() || (CurrentJob->Deadline && std::chrono::steady_clock::now() >= *CurrentJob->Deadline))
            {
                CurrentJob->Cancellation.Cancel();
                PyErr_SetString(CancelledError, "Script cancelled, session closed, or deadline exceeded; effects are not rolled back.");
                return true;
            }
            return false;
        }
        auto Trace(PyObject*, PyFrameObject* Frame, int Event, PyObject*) -> int
        {
            if (Event == PyTrace_CALL && PyObject_SetAttrString(reinterpret_cast<PyObject*>(Frame), "f_trace_opcodes", Py_True) < 0) return -1;
            return CheckCancelled() ? -1 : 0;
        }

        // Conversion bounds are enforced before traversing or copying children.
        struct FConversion
        {
            size_t Nodes = 0, Bytes = 0;
            std::unordered_set<PyObject*> Ancestors;
            auto From(PyObject* Object, const FToolSchema* Schema = nullptr, size_t Depth = 0) -> std::optional<FToolValue>
            {
                if (Depth > 32 || ++Nodes > 4096) { Fail("Tool value exceeds depth/node budget."); return {}; }
                if (Object == Py_None) return FToolValue{};
                if (PyBool_Check(Object)) return FToolValue(Object == Py_True);
                if (PyLong_CheckExact(Object))
                {
                    if (Schema && Schema->Type == EToolValueType::Number)
                    {
                        const double Value = PyLong_AsDouble(Object);
                        if (PyErr_Occurred() || !std::isfinite(Value)) return {};
                        return FToolValue(Value);
                    }
                    if (Schema && Schema->Type == EToolValueType::UnsignedInteger)
                    {
                        const auto Value = PyLong_AsUnsignedLongLong(Object);
                        if (PyErr_Occurred()) return {};
                        return FToolValue(static_cast<uint64>(Value));
                    }
                    int Overflow = 0; const auto Value = PyLong_AsLongLongAndOverflow(Object, &Overflow);
                    if (!Overflow && !PyErr_Occurred()) return FToolValue(static_cast<int64>(Value));
                    if (Overflow > 0 && (!Schema || Schema->Type != EToolValueType::Integer))
                    {
                        PyErr_Clear(); const auto Unsigned = PyLong_AsUnsignedLongLong(Object);
                        if (!PyErr_Occurred()) return FToolValue(static_cast<uint64>(Unsigned));
                    }
                    PyErr_SetString(PyExc_OverflowError, "Integer is outside the tool's 64-bit range."); return {};
                }
                if (PyFloat_CheckExact(Object))
                {
                    const double Value = PyFloat_AsDouble(Object);
                    if (!std::isfinite(Value)) { Fail("Non-finite tool number."); return {}; }
                    return FToolValue(Value);
                }
                if (PyUnicode_CheckExact(Object))
                {
                    Py_ssize_t Length = 0; const char* Text = PyUnicode_AsUTF8AndSize(Object, &Length);
                    if (!Text) return {};
                    Bytes += static_cast<size_t>(Length);
                    if (Length > 65536 || Bytes > 1024 * 1024) { Fail("Tool string budget exceeded."); return {}; }
                    return FToolValue(std::string(Text, static_cast<size_t>(Length)));
                }
                const bool IsObject = PyDict_CheckExact(Object), IsArray = PyList_CheckExact(Object);
                if (!IsObject && !IsArray) { Fail("Expected None, bool, int, float, str, list, or string-keyed dict."); return {}; }
                if (!Ancestors.insert(Object).second) { Fail("Cyclic tool value."); return {}; }
                struct FPop { std::unordered_set<PyObject*>& Set; PyObject* Object; ~FPop() { Set.erase(Object); } } Pop{Ancestors, Object};
                const auto Count = IsObject ? PyDict_Size(Object) : PyList_Size(Object);
                if (Count > 1024) { Fail("Tool container budget exceeded."); return {}; }
                if (IsArray)
                {
                    FToolValue::FArray Values;
                    for (Py_ssize_t I = 0; I < Count; ++I)
                    {
                        auto Value = From(PyList_GetItem(Object, I), Schema && Schema->Items ? Schema->Items.get() : nullptr, Depth + 1);
                        if (!Value) return {};
                        Values.push_back(std::move(*Value));
                    }
                    return FToolValue(std::move(Values));
                }
                FToolValue::FObject Values; PyObject* Key; PyObject* Item; Py_ssize_t Position = 0;
                while (PyDict_Next(Object, &Position, &Key, &Item))
                {
                    if (!PyUnicode_CheckExact(Key)) { Fail("Tool object keys must be strings."); return {}; }
                    auto Name = From(Key, nullptr, Depth + 1); if (!Name) return {};
                    const auto& Text = std::get<std::string>(Name->Data);
                    const FToolSchema* Child = nullptr;
                    if (Schema) if (auto It = Schema->Properties.find(Text); It != Schema->Properties.end()) Child = &It->second;
                    auto Value = From(Item, Child, Depth + 1); if (!Value) return {};
                    Values.emplace(Text, std::move(*Value));
                }
                return FToolValue(std::move(Values));
            }
        };
        auto ToPython(const FToolValue& Value) -> PyObject*
        {
            return std::visit([](const auto& Data) -> PyObject* {
                using T = std::decay_t<decltype(Data)>;
                if constexpr (std::is_same_v<T, std::monostate>) { Py_RETURN_NONE; }
                else if constexpr (std::is_same_v<T, bool>) return PyBool_FromLong(Data);
                else if constexpr (std::is_same_v<T, int64>) return PyLong_FromLongLong(Data);
                else if constexpr (std::is_same_v<T, uint64>) return PyLong_FromUnsignedLongLong(Data);
                else if constexpr (std::is_same_v<T, double>) return PyFloat_FromDouble(Data);
                else if constexpr (std::is_same_v<T, std::string>) return PyUnicode_DecodeUTF8(Data.data(), static_cast<Py_ssize_t>(Data.size()), "strict");
                else if constexpr (std::is_same_v<T, FToolValue::FArray>)
                {
                    FPy Result(PyList_New(0)); if (!Result) return nullptr;
                    for (const auto& Item : Data) { FPy Child(ToPython(Item)); if (!Child || PyList_Append(Result.get(), Child.get()) < 0) return nullptr; }
                    return Result.release();
                }
                else
                {
                    FPy Result(PyDict_New()); if (!Result) return nullptr;
                    for (const auto& [Key, Item] : Data)
                    {
                        FPy Name(PyUnicode_DecodeUTF8(Key.data(), static_cast<Py_ssize_t>(Key.size()), "strict"));
                        FPy Child(ToPython(Item));
                        if (!Name || !Child || PyDict_SetItem(Result.get(), Name.get(), Child.get()) < 0) return nullptr;
                    }
                    return Result.release();
                }
            }, Value.Data);
        }
        auto SchemaValue(const FToolSchema& Schema) -> FToolValue
        {
            FToolValue::FObject Data{{"type", static_cast<int64>(Schema.Type)}};
            Data.emplace("description", Schema.Description);
            if (Schema.Default) Data.emplace("default", *Schema.Default);
            if (Schema.Minimum) Data.emplace("minimum", *Schema.Minimum);
            if (Schema.Maximum) Data.emplace("maximum", *Schema.Maximum);
            if (Schema.MinSize) Data.emplace("min_size", static_cast<uint64>(*Schema.MinSize));
            if (Schema.MaxSize) Data.emplace("max_size", static_cast<uint64>(*Schema.MaxSize));
            if (!Schema.Properties.empty())
            {
                FToolValue::FObject Properties;
                for (const auto& [Name, Child] : Schema.Properties) Properties.emplace(Name, SchemaValue(Child));
                Data.emplace("properties", std::move(Properties));
            }
            FToolValue::FArray Required; for (const auto& Name : Schema.Required) Required.emplace_back(Name);
            Data.emplace("required", std::move(Required));
            if (Schema.Items) Data.emplace("items", SchemaValue(*Schema.Items));
            FToolValue::FArray Enum = Schema.Enum; Data.emplace("enum", std::move(Enum));
            return Data;
        }
        auto Diagnostics(const std::vector<FToolDiagnostic>& Values) -> FToolValue
        {
            FToolValue::FArray Result;
            for (const auto& D : Values) Result.emplace_back(FToolValue::FObject{{"code", static_cast<int64>(D.Code)}, {"path", D.Path}, {"message", D.Message}});
            return Result;
        }
        auto RaiseTool(const std::vector<FToolDiagnostic>& Values) -> PyObject*
        {
            FPy Error(PyObject_CallFunction(ToolError, "s", Values.empty() ? "Tool invocation failed." : Values.front().Message.c_str()));
            FPy Details(ToPython(Diagnostics(Values)));
            if (Error && Details && PyObject_SetAttrString(Error.get(), "diagnostics", Details.get()) == 0) PyErr_SetObject(ToolError, Error.get());
            return nullptr;
        }
        auto FindTool(const char* Toolset, const char* Tool) -> std::optional<FToolDescriptor>
        {
            auto Catalog = CurrentJob->Endpoint->GetCatalog();
            for (const auto& Set : Catalog.Toolsets) if (Set.Descriptor.Name == Toolset)
                for (const auto& Descriptor : Set.Descriptor.Tools)
                    if (Descriptor.Name == Tool && (CurrentJob->Source != EToolSource::External || Descriptor.bExposeExternally)) return Descriptor;
            PyErr_SetString(ToolError, "Unknown, unavailable, or unexposed tool."); return {};
        }
        auto FinishScope(FJob::FScope& Scope, bool Cancel) -> std::string
        {
            const bool Outstanding = Scope.Native->IsOutstanding();
            if (Cancel) Scope.Native->Cancel(); else (void)Scope.Native->End();
            auto* Owner = Scope.Owner.Get();
            if (!Outstanding || !Owner) return {};
            const auto& Result = Owner->GetLastResult();
            using Editor::ETransactorResultCode;
            if (Result.Code == ETransactorResultCode::Failed || Result.Code == ETransactorResultCode::RecoveryRequired
                || Result.Code == ETransactorResultCode::Rejected) return Editor::FormatTransactorResult(Result);
            return {};
        }
        auto RequestImpl(PyObject*, PyObject* Args) -> PyObject*
        {
            const char* Operation; PyObject* Params;
            if (!PyArg_ParseTuple(Args, "sO!", &Operation, &PyTuple_Type, &Params)) return nullptr;
            const std::string_view Op(Operation);
            const bool TransactionCleanup = Op == "transaction_end" || Op == "transaction_cancel";
            const bool Cleanup = TransactionCleanup || (CurrentJob && CurrentJob->Cleaning && (Op == "write" || Op == "owner"));
            if (!CurrentJob) { PyErr_SetString(PyExc_RuntimeError, "No active Durin script job."); return nullptr; }
            if (CurrentJob->Cleaning && !Cleanup) { PyErr_SetString(CancelledError, "New work is forbidden during job cleanup."); return nullptr; }
            if (!Cleanup && CheckCancelled()) return nullptr;
            if (Op == "reflect_type")
            {
                const char* Name; if (!PyArg_ParseTuple(Params, "s", &Name)) return nullptr;
                return PythonReflection::GetType(Name);
            }
            if (Op == "editor_subsystem")
            {
                PyObject* Type; if (!PyArg_ParseTuple(Params, "O", &Type)) return nullptr;
                return PythonReflection::GetEditorSubsystem(Type);
            }
            if (Op == "reflection_stubs")
            {
                if (!PyArg_ParseTuple(Params, "")) return nullptr;
                return PythonReflection::GenerateStubs();
            }
            if (Op == "transaction_begin")
            {
                const char* Description;
                if (!PyArg_ParseTuple(Params, "s", &Description)) return nullptr;
                const auto Id = NextIdentity++;
                auto* Owner = CurrentEditor->GetTransactor();
                auto Scope = std::make_unique<Editor::FScopedTransaction>(Owner, Editor::FTransactionContext{.Description = Description});
                if (!Scope->IsOutstanding()) return Fail("The editor rejected transaction recording.");
                CurrentJob->Scopes.push_back({Id, std::move(Scope), TWeakObjectPtr<DTransactor>(Owner)});
                return Py_BuildValue("KK", static_cast<unsigned long long>(CurrentJob->Id), static_cast<unsigned long long>(Id));
            }
            if (TransactionCleanup)
            {
                unsigned long long Owner, Id;
                if (!PyArg_ParseTuple(Params, "KK", &Owner, &Id)) return nullptr;
                if (Owner != CurrentJob->Id) return Fail("Transaction belongs to another script job.");
                const auto It = std::ranges::find(CurrentJob->Scopes, Id, [](const auto& Scope) { return Scope.Id; });
                if (It != CurrentJob->Scopes.end())
                {
                    const auto Error = FinishScope(*It, Op == "transaction_cancel");
                    CurrentJob->Scopes.erase(It);
                    if (!Error.empty()) { PyErr_SetString(PyExc_RuntimeError, Error.c_str()); return nullptr; }
                }
                Py_RETURN_NONE;
            }
            if (Op == "defer")
            {
                PyObject* Work;
                if (!PyArg_ParseTuple(Params, "O", &Work)) return nullptr;
                if (CurrentJob->Continuations.size() + (CurrentJob->InContinuation ? 1 : 0) >= 256) return Fail("Script continuation budget exceeded.");
                Py_IncRef(Work); CurrentJob->Continuations.push_back({FPy(Work)});
                Py_RETURN_NONE;
            }
            if (Op == "owner")
            {
                unsigned long long Owner;
                if (!PyArg_ParseTuple(Params, "K", &Owner)) return nullptr;
                if (Owner != CurrentJob->Id) return Fail("Tools facade belongs to another script job/session.");
                Py_RETURN_NONE;
            }
            if (Op == "write")
            {
                const char* Text; Py_ssize_t Size;
                if (!PyArg_ParseTuple(Params, "s#", &Text, &Size)) return nullptr;
                std::lock_guard Lock(CurrentJob->Mutex);
                const size_t Available = MaxOutput - CurrentJob->Result.Output.size();
                CurrentJob->Result.Output.append(Text, Utf8Prefix(std::string_view(Text, static_cast<size_t>(Size)), Available));
                if (static_cast<size_t>(Size) > Available)
                { CurrentJob->OutputOverflow = true; PyErr_SetString(PyExc_RuntimeError, "Script output exceeded 64 KiB."); return nullptr; }
                return PyLong_FromSsize_t(Size);
            }
            if (Op == "list")
            {
                if (!PyArg_ParseTuple(Params, "")) return nullptr;
                FToolValue::FArray Values;
                for (const auto& Set : CurrentJob->Endpoint->GetCatalog().Toolsets)
                    for (const auto& Tool : Set.Descriptor.Tools)
                        if (CurrentJob->Source != EToolSource::External || Tool.bExposeExternally)
                            Values.emplace_back(FToolValue::FObject{{"toolset", Set.Descriptor.Name}, {"tool", Tool.Name}, {"version", static_cast<uint64>(Tool.ContractVersion)}, {"description", Tool.Description}});
                return ToPython(Values);
            }
            if (Op == "describe" || Op == "submit" || Op == "call")
            {
                const char* Set; const char* Name; PyObject* Input = nullptr; PyObject* Version = Py_None;
                if (Op == "describe") { if (!PyArg_ParseTuple(Params, "ss", &Set, &Name)) return nullptr; }
                else if (!PyArg_ParseTuple(Params, "ssOO", &Set, &Name, &Input, &Version)) return nullptr;
                auto Descriptor = FindTool(Set, Name); if (!Descriptor) return nullptr;
                if (Op == "describe") return ToPython(FToolValue::FObject{{"toolset", Set}, {"tool", Name},
                    {"version", static_cast<uint64>(Descriptor->ContractVersion)}, {"description", Descriptor->Description},
                    {"input", SchemaValue(Descriptor->Input)}, {"output", SchemaValue(Descriptor->Output)}});
                if (Op == "submit" && CurrentJob->Invocations.size() >= 256) return Fail("Script invocation budget exceeded.");
                if (!PyDict_CheckExact(Input)) return Fail("Tool arguments must be a dict.");
                FConversion Conversion; auto Value = Conversion.From(Input, &Descriptor->Input); if (!Value) return nullptr;
                uint32 Contract = Descriptor->ContractVersion;
                if (Version != Py_None)
                {
                    if (!PyLong_CheckExact(Version)) return Fail("Version must be a positive integer.");
                    const auto V = PyLong_AsUnsignedLong(Version);
                    if (PyErr_Occurred()) return nullptr;
                    if (!V || V > std::numeric_limits<uint32>::max()) return Fail("Version outside uint32 range.");
                    Contract = static_cast<uint32>(V);
                }
                auto* Tools = CurrentEditor->GetEditorSubsystem<DEditorToolSubsystem>();
                if (!Tools) { PyErr_SetString(ToolError, "Editor tools are unavailable."); return nullptr; }
                FToolRequest Request{{Set, Name}, Contract, std::move(*Value)};
                if (Op == "call")
                {
                    auto Result = Tools->CallNow(Request, CurrentJob->Source, CurrentJob->Client, CurrentJob->Cancellation.GetToken());
                    if (CheckCancelled()) return nullptr;
                    return Result.Status == EToolStatus::Succeeded ? ToPython(Result.Data) : RaiseTool(Result.Diagnostics);
                }
                auto Submitted = Tools->SubmitNow(Request, CurrentJob->Source, CurrentJob->Client, CurrentJob->Cancellation.GetToken());
                if (!Submitted) return RaiseTool({Submitted.error()});
                const uint64 Id = Submitted->Id.Sequence;
                CurrentJob->Invocations.emplace(Id, std::move(*Submitted));
                if (CheckCancelled()) return nullptr;
                return Py_BuildValue("KK", static_cast<unsigned long long>(CurrentJob->Id), static_cast<unsigned long long>(Id));
            }
            if (Op == "state" || Op == "cancel" || Op == "result")
            {
                unsigned long long Owner, Id;
                if (!PyArg_ParseTuple(Params, "KK", &Owner, &Id)) return nullptr;
                if (Owner != CurrentJob->Id) return Fail("Invocation belongs to another script job/session.");
                auto It = CurrentJob->Invocations.find(Id);
                if (It == CurrentJob->Invocations.end()) return Fail("Unknown invocation handle.");
                if (Op == "cancel")
                { (void)CurrentJob->Endpoint->Cancel(It->second.Id); Py_RETURN_NONE; }
                auto Result = It->second.Future.GetResultShared();
                if (Op == "state") return PyUnicode_FromString(Result ? "completed" : "running");
                if (!Result) { PyErr_SetString(NotReady, "Invocation has not completed; yield the handle to wait cooperatively."); return nullptr; }
                return Result->Status == EToolStatus::Succeeded ? ToPython(Result->Data) : RaiseTool(Result->Diagnostics);
            }
            return Fail("Unknown Durin binding operation.");
        }
        auto Request(PyObject* Self, PyObject* Args) -> PyObject*
        {
            try { return RequestImpl(Self, Args); }
            catch (const std::exception& Error) { PyErr_SetString(PyExc_RuntimeError, Error.what()); return nullptr; }
            catch (...) { PyErr_SetString(PyExc_RuntimeError, "Native Python adapter failure."); return nullptr; }
        }
        PyMethodDef Methods[] = {{"_request", Request, METH_VARARGS, nullptr}, {nullptr, nullptr, 0, nullptr}};
        PyModuleDef Module = {PyModuleDef_HEAD_INIT, "_durin", nullptr, -1, Methods};
        auto InitModule() -> PyObject*
        {
            FPy Result(PyModule_Create(&Module)); if (!Result) return nullptr;
            ToolError = PyErr_NewException("durin.ToolError", PyExc_RuntimeError, nullptr);
            CancelledError = PyErr_NewException("durin.CancelledError", PyExc_BaseException, nullptr);
            ScriptAccessError = PyErr_NewException("durin.ScriptAccessError", PyExc_RuntimeError, nullptr);
            NotReady = PyErr_NewException("durin.NotReady", PyExc_RuntimeError, nullptr);
            if (!ToolError || !CancelledError || !NotReady || !ScriptAccessError) return nullptr;
            if (PyModule_AddObjectRef(Result.get(), "ToolError", ToolError) < 0 ||
                PyModule_AddObjectRef(Result.get(), "CancelledError", CancelledError) < 0 ||
                PyModule_AddObjectRef(Result.get(), "NotReady", NotReady) < 0 ||
                PyModule_AddObjectRef(Result.get(), "ScriptAccessError", ScriptAccessError) < 0) return nullptr;
            return Result.release();
        }
        auto PythonError() -> std::string
        {
            FPy Error(PyErr_GetRaisedException());
            if (!Error) return "Python operation failed.";
            FPy ModuleObject(PyImport_ImportModule("traceback"));
            FPy Lines(ModuleObject ? PyObject_CallMethod(ModuleObject.get(), "format_exception", "O", Error.get()) : nullptr);
            FPy Separator(PyUnicode_FromString(""));
            FPy Text(Lines && Separator ? PyUnicode_Join(Separator.get(), Lines.get()) : PyObject_Str(Error.get()));
            std::string Result = "Python exception (format unavailable).";
            if (Text) { const char* Value = PyUnicode_AsUTF8(Text.get()); if (Value) Result = Value; }
            PyErr_Clear(); if (Result.size() > MaxOutput) Result.resize(Utf8Prefix(Result, MaxOutput)); return Result;
        }
    }
#endif

    struct FPythonRuntime::FImpl : IEditorScriptPump
    {
        struct FSession { std::shared_ptr<FEditorToolService> Endpoint; EToolSource Source; FToolClientContext Client; FPythonSessionOptions Options; };
        mutable std::mutex Mutex;
        std::map<uint64, FSession> Sessions;
        std::map<uint64, std::shared_ptr<FJob>> Jobs;
        std::deque<std::shared_ptr<FJob>> Queue;
        bool Executing = false;
        uint64 PumpSerial = 0;
        FModularFeatureRegistration Registration;
#if DURIN_WITH_PYTHON
        PyThreadState* ThreadState = nullptr;
        bool OwnsInterpreter = true;
        auto ReleaseInterpreter() -> void
        {
            if (std::exchange(OwnsInterpreter, false)) InterpreterOwned = false;
        }
#endif
        bool Stopping = false;
        std::filesystem::path Directory;
        std::optional<std::chrono::milliseconds> JobDeadline;

        auto Cancel(const std::shared_ptr<FJob>& Job) -> void
        {
            Job->Cancellation.Cancel();
            std::lock_guard Lock(Job->Mutex);
            if (Job->Result.State == EPythonJobState::Queued)
            {
                Job->Result.State = EPythonJobState::Cancelled; Job->Endpoint.reset();
                std::erase(Queue, Job); Job->Code.clear();
            }
        }
        // Caller holds Mutex. Client lifetimes are detached cancellation identities, not callbacks.
        auto PruneClients() -> void
        {
            std::erase_if(Sessions, [](const auto& Entry) { return Entry.second.Client.Cancellation.IsCancellationRequested() || !Entry.second.Endpoint->GetWorkGate()->IsOpen(); });
            for (const auto& [Id, Job] : Jobs)
            {
                (void)Id;
                bool Active;
                { std::lock_guard Lock(Job->Mutex); Active = !Terminal(Job->Result.State); }
                if (!Active) continue;
                const bool Expired = Job->Deadline && std::chrono::steady_clock::now() >= *Job->Deadline;
                if (!Expired && !Job->Client.Cancellation.IsCancellationRequested() && Job->Gate->IsOpen()) continue;
                if (Expired) { std::lock_guard JobLock(Job->Mutex); if (Job->Result.Error.empty()) Job->Result.Error = "Script deadline exceeded; effects are not rolled back."; }
                Cancel(Job);
            }
        }
#if DURIN_WITH_PYTHON
        auto AppendError(FJob& Job, std::string Error) -> void
        {
            std::lock_guard Lock(Job.Mutex);
            if (!Job.Result.Error.empty()) Job.Result.Error += "\n";
            Job.Result.Error += Error;
            if (Job.Result.Error.size() > MaxOutput) Job.Result.Error.resize(Utf8Prefix(Job.Result.Error, MaxOutput));
        }
        auto Settled(const FJob& Job) const -> bool
        {
            return std::ranges::all_of(Job.Invocations, [](const auto& Entry) { return Entry.second.Future.IsCompleted(); });
        }
        auto Runnable(const FJob& Job) const -> bool
        {
            if (Job.Cancellation.GetToken().IsCancellationRequested()) return true;
            if (!Job.Started) return true;
            if (Job.Continuations.empty()) return Settled(Job);
            const auto Id = Job.Continuations.front().Waiting;
            return !Id || Job.Invocations.at(Id).Future.IsCompleted();
        }
        auto Step(FJob& Job) -> bool
        {
            if (!Job.Started)
            {
                Job.Started = true; Job.Globals.reset(PyDict_New());
                FPy Name(PyUnicode_FromString("__main__"));
                FPy File(PyUnicode_DecodeUTF8(Job.Filename.data(), static_cast<Py_ssize_t>(Job.Filename.size()), "strict"));
                if (!Job.Globals || !Name || !File || PyDict_SetItemString(Job.Globals.get(), "__name__", Name.get()) < 0
                    || PyDict_SetItemString(Job.Globals.get(), "__file__", File.get()) < 0) return false;
                FPy Source(PyUnicode_DecodeUTF8(Job.Code.data(), static_cast<Py_ssize_t>(Job.Code.size()), "strict"));
                FPy Code(Source ? Py_CompileString(Job.Code.c_str(), Job.Filename.c_str(), Py_file_input) : nullptr);
                FPy Result(Code ? PyEval_EvalCode(Code.get(), Job.Globals.get(), Job.Globals.get()) : nullptr);
                return static_cast<bool>(Result);
            }
            if (Job.Continuations.empty()) return true;
            auto Continuation = std::move(Job.Continuations.front()); Job.Continuations.pop_front();
            Job.InContinuation = true;
            struct FStepExit { FJob& Job; ~FStepExit() { Job.InContinuation = false; } } StepExit{Job};
            auto Failed = [&]() { Job.Continuations.push_front(std::move(Continuation)); return false; };
            FPy Value, Error;
            if (Continuation.Waiting)
            {
                auto Result = Job.Invocations.at(Continuation.Waiting).Future.GetResultShared();
                if (!Result) return Failed();
                if (Result->Status == EToolStatus::Succeeded) Value.reset(ToPython(Result->Data));
                else { (void)RaiseTool(Result->Diagnostics); Error.reset(PyErr_GetRaisedException()); }
                if (PyErr_Occurred()) return Failed();
            }
            FPy Result(PyObject_CallMethod(Continuation.Object.get(), "step", "OO", Value ? Value.get() : Py_None, Error ? Error.get() : Py_None));
            int Done = 0; unsigned long long Owner = 0, Id = 0;
            if (!Result || !PyArg_ParseTuple(Result.get(), "pKK", &Done, &Owner, &Id)) return Failed();
            if (Id && (Owner != Job.Id || !Job.Invocations.contains(Id))) { Fail("Yielded invocation belongs to another job."); return Failed(); }
            if (!Done) { Continuation.Waiting = Id; Job.Continuations.push_front(std::move(Continuation)); }
            return true;
        }
        // Every entry binds the full job facade/output/trace context, including close/finally.
        auto Enter(FJob& Job, bool Cleanup) -> bool
        {
            CurrentJob = &Job; Job.Cleaning = Cleanup;
            FPy Sys(PyImport_ImportModule("sys")), Durin(PyImport_ImportModule("durin"));
            FPy OldOut(Sys ? PyObject_GetAttrString(Sys.get(), "stdout") : nullptr);
            FPy OldErr(Sys ? PyObject_GetAttrString(Sys.get(), "stderr") : nullptr);
            FPy OldTools(Durin ? PyObject_GetAttrString(Durin.get(), "tools") : nullptr);
            if (!Job.Tools && Durin) Job.Tools.reset(PyObject_CallMethod(Durin.get(), "_Tools", "K", static_cast<unsigned long long>(Job.Id)));
            if (!Job.Output && Durin) Job.Output.reset(PyObject_CallMethod(Durin.get(), "_Output", nullptr));
            bool Ready = Sys && Durin && OldOut && OldErr && OldTools && Job.Tools && Job.Output;
            if (Ready) Ready = PyObject_SetAttrString(Durin.get(), "tools", Job.Tools.get()) == 0
                && PyObject_SetAttrString(Sys.get(), "stdout", Job.Output.get()) == 0 && PyObject_SetAttrString(Sys.get(), "stderr", Job.Output.get()) == 0;
            bool Success = Ready;
            try
            {
                if (Cleanup)
                {
                    for (const auto& [Id, Invocation] : Job.Invocations) { (void)Id; (void)Job.Endpoint->Cancel(Invocation.Id); }
                    while (!Job.Continuations.empty())
                    {
                        auto Continuation = std::move(Job.Continuations.front()); Job.Continuations.pop_front();
                        FPy Closed(PyObject_CallMethod(Continuation.Object.get(), "close", nullptr));
                        if (!Closed) { AppendError(Job, "Generator cleanup failed: " + PythonError()); Success = false; }
                    }
                    Job.Globals.reset();
                    if (PyGC_Collect() < 0 || PyErr_Occurred()) { AppendError(Job, "Python reference cleanup failed: " + PythonError()); Success = false; }
                    while (!Job.Scopes.empty())
                    {
                        const auto Error = FinishScope(Job.Scopes.back(), false); Job.Scopes.pop_back();
                        if (!Error.empty()) { AppendError(Job, "Transaction cleanup failed: " + Error); Success = false; }
                    }
                }
                else if (Ready)
                {
                    PyEval_SetTrace(Trace, nullptr);
                    Success = Step(Job);
                    PyEval_SetTrace(nullptr, nullptr);
                }
            }
            catch (const std::exception& Error) { PyEval_SetTrace(nullptr, nullptr); AppendError(Job, Error.what()); Success = false; }
            catch (...) { PyEval_SetTrace(nullptr, nullptr); AppendError(Job, "Native continuation failure."); Success = false; }
            if (!Success && PyErr_Occurred()) AppendError(Job, PythonError());
            if (Job.OutputOverflow) { AppendError(Job, "Script output exceeded 64 KiB."); Success = false; }
            if (Sys && OldOut) (void)PyObject_SetAttrString(Sys.get(), "stdout", OldOut.get());
            if (Sys && OldErr) (void)PyObject_SetAttrString(Sys.get(), "stderr", OldErr.get());
            if (Durin && OldTools) (void)PyObject_SetAttrString(Durin.get(), "tools", OldTools.get());
            if (Cleanup) { Job.Tools.reset(); Job.Output.reset(); Job.Invocations.clear(); Job.Code.clear(); Job.Started = false; }
            PyErr_Clear(); Job.Cleaning = false; CurrentJob = nullptr;
            return Success;
        }
        auto Finish(FJob& Job, EPythonJobState Outcome) -> void
        {
            if (!Enter(Job, true) && Outcome == EPythonJobState::Succeeded) Outcome = EPythonJobState::Failed;
            Job.Endpoint.reset();
            std::lock_guard Lock(Job.Mutex);
            Job.Result.State = Job.Cancellation.GetToken().IsCancellationRequested() || Job.Client.Cancellation.IsCancellationRequested() || !Job.Gate->IsOpen()
                ? EPythonJobState::Cancelled : Outcome;
        }
        auto Initialize() -> std::string
        {
            static std::once_flag Registration;
            std::call_once(Registration, [] { PyImport_AppendInittab("_durin", InitModule); });
            PyConfig Config; PyConfig_InitIsolatedConfig(&Config);
            Config.site_import = 0; Config.write_bytecode = 0; Config.install_signal_handlers = 0;
            Config.parse_argv = 0; Config.module_search_paths_set = 1;
            auto Zip = (Directory / "python313.zip").wstring();
            PyStatus Status = PyWideStringList_Append(&Config.module_search_paths, Zip.c_str());
            auto Package = (Directory / "durin-python.zip").wstring();
            if (!PyStatus_Exception(Status)) Status = PyWideStringList_Append(&Config.module_search_paths, Package.c_str());
            if (!PyStatus_Exception(Status)) Status = Py_InitializeFromConfig(&Config);
            const std::string Error = PyStatus_Exception(Status)
                ? (Status.err_msg ? Status.err_msg : "CPython initialization failed.") : "";
            PyConfig_Clear(&Config);
            if (!Error.empty())
            {
                if (Py_IsInitialized()) Py_FinalizeEx();
                return Error;
            }
            FPy Durin(PyImport_ImportModule("durin"));
            if (Durin && !PythonReflection::Start(Durin.get(), ScriptAccessError, []() -> std::optional<FEditorScriptCallContext> {
                if (!CurrentJob || CurrentJob->Cleaning || !IsValid(CurrentEditor)) return {};
                if (CheckCancelled()) return {};
                return FEditorScriptCallContext{CurrentEditor, CurrentJob->Gate,
                    static_cast<EEditorScriptEditPolicy>(CurrentJob->Options.EditPolicy),
                    CurrentJob->Cancellation.GetToken(), CurrentJob->Client, CurrentJob->Source};
            })) Durin.reset();
            if (!Durin)
            {
                auto Error = PythonError(); Durin.reset();
                PythonReflection::Stop(); Py_CLEAR(ScriptAccessError); Py_CLEAR(ToolError); Py_CLEAR(CancelledError); Py_CLEAR(NotReady); Py_FinalizeEx(); return Error;
            }
            Durin.reset(); ThreadState = PyEval_SaveThread(); return {};
        }
        auto Finalize() -> void
        {
            if (!ThreadState) return;
            PyEval_RestoreThread(std::exchange(ThreadState, nullptr));
            CurrentEditor = nullptr;
            for (auto& [Id, Job] : Jobs)
            {
                (void)Id;
                if (Job->Started) { Job->Cancellation.Cancel(); Finish(*Job, EPythonJobState::Cancelled); }
            }
            PythonReflection::Stop(); Py_CLEAR(ScriptAccessError); Py_CLEAR(ToolError); Py_CLEAR(CancelledError); Py_CLEAR(NotReady);
            if (Py_FinalizeEx() < 0) DURIN_ERROR("Python finalization reported cleanup failures.");
            ReleaseInterpreter();
        }
#endif
        auto CloseEndpoint(const std::shared_ptr<FEditorToolService>& Endpoint) -> void override
        {
#if DURIN_WITH_PYTHON
            PythonReflection::PurgeGate(Endpoint->GetWorkGate().get());
            std::vector<std::shared_ptr<FJob>> Closing;
            {
                std::lock_guard Lock(Mutex);
                for (const auto& [Id, Job] : Jobs) if (Job->Endpoint == Endpoint) { Cancel(Job); Closing.push_back(Job); }
            }
            if (Executing || !ThreadState) return;
            Executing = true; PyEval_RestoreThread(std::exchange(ThreadState, nullptr)); CurrentEditor = nullptr;
            for (auto& Job : Closing) if (Job->Started) { Finish(*Job, EPythonJobState::Cancelled); std::lock_guard Lock(Mutex); std::erase(Queue, Job); }
            ThreadState = PyEval_SaveThread(); Executing = false;
#else
            (void)Endpoint;
#endif
        }
        auto Pump(DEditorEngine& Editor, const std::shared_ptr<FEditorToolService>& Endpoint) -> void override
        {
#if DURIN_WITH_PYTHON
            if (Executing || !ThreadState) return;
            Executing = true;
            PyEval_RestoreThread(std::exchange(ThreadState, nullptr));
            size_t Remaining;
            { std::lock_guard Lock(Mutex); PruneClients(); Remaining = Queue.size(); }
            const auto Start = std::chrono::steady_clock::now();
            const auto Round = ++PumpSerial;
            for (size_t Steps = 0; Remaining-- && Steps < 16 && (Steps == 0 || std::chrono::steady_clock::now() - Start < std::chrono::milliseconds(2));)
            {
                std::shared_ptr<FJob> Job;
                {
                    std::lock_guard Lock(Mutex);
                    if (Queue.empty()) break;
                    Job = Queue.front(); Queue.pop_front();
                    if (Job->LastPump == Round || (Job->Endpoint != Endpoint && !Job->Cancellation.GetToken().IsCancellationRequested()) || !Runnable(*Job))
                    { Queue.push_back(Job); continue; }
                    std::lock_guard JobLock(Job->Mutex); Job->Result.State = EPythonJobState::Running;
                }
                ++Steps; Job->LastPump = Round; CurrentEditor = Job->Endpoint == Endpoint ? &Editor : nullptr;
                const bool Published = PythonReflection::Publish();
                if (!Published) { const auto Error = PythonError(); std::lock_guard Lock(Job->Mutex); Job->Result.Error = Error; }
                bool Cancelled = Job->Cancellation.GetToken().IsCancellationRequested();
                const bool Success = Cancelled || !Published ? false : Enter(*Job, false);
                Cancelled = Job->Cancellation.GetToken().IsCancellationRequested();
                if (!Success || Cancelled || (Job->Continuations.empty() && Settled(*Job)))
                    Finish(*Job, Success ? EPythonJobState::Succeeded : EPythonJobState::Failed);
                else { std::lock_guard Lock(Mutex); Queue.push_back(Job); }
                CurrentEditor = nullptr;
            }
            ThreadState = PyEval_SaveThread(); Executing = false;
            if (Stopping) Finalize();
#else
            (void)Editor; (void)Endpoint;
#endif
        }
    };

    FPythonRuntime::FPythonRuntime() = default;
    FPythonRuntime::~FPythonRuntime() { require(IsInGameThread()); Stop(); }
    auto FPythonRuntime::Start(const FPythonRuntimeOptions& Options) -> std::expected<void, std::string>
    {
        if (!IsInGameThread() || FModularFeatureRegistry::Get().IsDispatching())
            return std::unexpected("Python Start requires a GameThread safe point outside provider dispatch.");
        if (Options.JobDeadline && Options.JobDeadline->count() <= 0) return std::unexpected("JobDeadline must be positive when set.");
        std::unique_lock LifecycleLock(LifecycleMutex);
        if (Impl && Impl->Stopping && !Impl->Executing) StopLocked();
        if (Impl) return std::unexpected("Python runtime already started.");
#if DURIN_WITH_PYTHON
        bool Expected = false;
        if (!InterpreterOwned.compare_exchange_strong(Expected, true)) return std::unexpected("Another Durin Python runtime owns the interpreter.");
        auto Directory = Options.RuntimeDirectory;
        if (Directory.empty())
        {
            Directory = std::filesystem::u8path(FPlatformProcess::ExecutablePath()).parent_path();
            if (!std::filesystem::exists(Directory / "python313.zip")) Directory = FPaths::EngineThirdPartyRuntimeBinariesDir();
        }
        if (!std::filesystem::exists(Directory / "python313.zip") || !std::filesystem::exists(Directory / DURIN_PYTHON_LIBRARY) ||
            !std::filesystem::exists(Directory / "durin-python.zip"))
        { InterpreterOwned = false; return std::unexpected("Packaged Python runtime is missing; prepare python and rebuild DurinPython."); }
        // Resolve only the qualified DLL; retain it for process lifetime.
        const auto Library = FPlatformLibrary::Load(Directory / DURIN_PYTHON_LIBRARY);
        if (!Library)
        { InterpreterOwned = false; return std::unexpected(Library.error().ToString()); }
        const auto Symbols = PythonABI::API.Load(*Library);
        if (!Symbols)
        { InterpreterOwned = false; return std::unexpected(Symbols.error().ToString()); }
        if (Py_IsInitialized())
        { InterpreterOwned = false; return std::unexpected("An existing Python interpreter is not owned by Durin."); }
        Impl = std::make_unique<FImpl>(); Impl->Directory = std::move(Directory); Impl->JobDeadline = Options.JobDeadline;
        auto Error = Impl->Initialize();
        if (!Error.empty()) { StopLocked(); return std::unexpected(std::move(Error)); }
        Impl->Registration = FModularFeatureRegistry::Get().RegisterFeature<IEditorScriptPump>(*Impl);
        return {};
#else
        (void)Options;
        return std::unexpected("Python embedding is unavailable on this host or the qualified SDK was not prepared.");
#endif
    }
    auto FPythonRuntime::Stop() noexcept -> void
    {
        require(IsInGameThread());
        std::unique_lock LifecycleLock(LifecycleMutex);
        StopLocked();
    }
    auto FPythonRuntime::StopLocked() noexcept -> void
    {
        if (!Impl) return;
        {
            std::lock_guard Lock(Impl->Mutex); Impl->Stopping = true; Impl->Sessions.clear();
            for (const auto& [Id, Job] : Impl->Jobs) { (void)Id; Impl->Cancel(Job); }
        }
        // Stop called by a native handler only signals cancellation. The pump
        // finalizes after that handler and the script have actually returned.
        if (Impl->Executing || FModularFeatureRegistry::Get().IsDispatching()) return;
#if DURIN_WITH_PYTHON
        Impl->Finalize(); Impl->ReleaseInterpreter();
#endif
        Impl->Registration.Reset(); Impl.reset();
    }
    auto FPythonRuntime::CreateSession(std::shared_ptr<FEditorToolService> Endpoint, EToolSource Source, const FToolClientContext& Client, const FPythonSessionOptions& Options) -> std::expected<uint64, std::string>
    {
        std::shared_lock LifecycleLock(LifecycleMutex);
        if (!Impl || Impl->Stopping || !Endpoint || !Endpoint->GetWorkGate()->IsOpen()) return std::unexpected("Python runtime or editor endpoint is unavailable.");
        std::lock_guard Lock(Impl->Mutex);
        Impl->PruneClients();
        if (Client.Cancellation.IsCancellationRequested()) return std::unexpected("Python client session is closed.");
        if (Impl->Stopping || Impl->Sessions.size() >= 8) return std::unexpected("Python session admission is closed or full.");
        const uint64 Id = NextIdentity++; Impl->Sessions.emplace(Id, FImpl::FSession{std::move(Endpoint), Source, Client, Options}); return Id;
    }
    auto FPythonRuntime::CloseSession(uint64 Session) -> void
    {
        std::shared_lock LifecycleLock(LifecycleMutex);
        if (!Impl) return;
        std::lock_guard Lock(Impl->Mutex); Impl->Sessions.erase(Session);
        for (const auto& [Id, Job] : Impl->Jobs) { (void)Id; if (Job->Session == Session) Impl->Cancel(Job); }
    }
    auto FPythonRuntime::RunString(uint64 Session, std::string Code, std::string Filename) -> std::expected<uint64, std::string>
    {
        std::shared_lock LifecycleLock(LifecycleMutex);
        if (!Impl || Impl->Stopping) return std::unexpected("Python runtime is unavailable.");
        if (Code.size() > MaxSource || Code.find('\0') != std::string::npos || Filename.size() > 4096 || Filename.find('\0') != std::string::npos)
            return std::unexpected("Script source/filename exceeds limits or contains NUL.");
        std::lock_guard Lock(Impl->Mutex);
        Impl->PruneClients();
        auto It = Impl->Sessions.find(Session);
        if (Impl->Stopping || It == Impl->Sessions.end()) return std::unexpected("Python session is closed or unknown.");
        size_t Outstanding = 0;
        for (const auto& [Id, Job] : Impl->Jobs) { (void)Id; std::lock_guard JobLock(Job->Mutex); if (!Terminal(Job->Result.State)) ++Outstanding; }
        if (Outstanding >= 16) return std::unexpected("Python job queue is full.");
        while (Impl->Jobs.size() >= 64)
        {
            auto Victim = std::ranges::find_if(Impl->Jobs, [](const auto& Entry) { std::lock_guard JobLock(Entry.second->Mutex); return Terminal(Entry.second->Result.State); });
            if (Victim == Impl->Jobs.end()) return std::unexpected("Python job retention is full.");
            Impl->Jobs.erase(Victim);
        }
        auto Job = std::make_shared<FJob>(); Job->Id = NextIdentity++; Job->Session = Session;
        Job->Endpoint = It->second.Endpoint; Job->Gate = Job->Endpoint->GetWorkGate(); Job->Source = It->second.Source;
        Job->Client = It->second.Client; Job->Options = It->second.Options;
        if (Impl->JobDeadline)
        {
            const auto Now = std::chrono::steady_clock::now();
            if (*Impl->JobDeadline > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - Now))
                return std::unexpected("JobDeadline exceeds the clock range.");
            Job->Deadline = Now + *Impl->JobDeadline;
        }
        Job->Code = std::move(Code); Job->Filename = std::move(Filename);
        Impl->Jobs.emplace(Job->Id, Job); Impl->Queue.push_back(Job); return Job->Id;
    }
    auto FPythonRuntime::RunFile(uint64 Session, const std::filesystem::path& Path) -> std::expected<uint64, std::string>
    {
        if (Path.empty()) return std::unexpected("Script path is empty.");
        auto Resolved = Path;
        if (!Resolved.is_absolute())
        {
            if (Resolved.has_root_path()) return std::unexpected("Relative scripts must not specify a drive or root.");
            const auto Project = std::filesystem::path(FPaths::ProjectDir());
            if (!Project.is_absolute()) return std::unexpected("An absolute project directory is required for relative scripts.");
            Resolved = Project / Resolved;
        }
        Resolved = Resolved.lexically_normal();
        const auto Filename = Resolved.u8string();
        if (Filename.size() > 4096 || Filename.find(u8'\0') != std::u8string::npos) return std::unexpected("Invalid Python script path.");
        std::ifstream Stream(Resolved, std::ios::binary);
        if (!Stream) return std::unexpected("Could not open Python script.");
        std::string Source(MaxSource + 1, '\0'); Stream.read(Source.data(), static_cast<std::streamsize>(Source.size()));
        const auto Count = Stream.gcount();
        if (Stream.bad() || Count > static_cast<std::streamsize>(MaxSource)) return std::unexpected("Script read failed or source exceeds 1 MiB.");
        Source.resize(static_cast<size_t>(Count));
        return RunString(Session, std::move(Source), std::string(reinterpret_cast<const char*>(Filename.data()), Filename.size()));
    }
    auto FPythonRuntime::GetJobResult(uint64 Job) const -> std::expected<FPythonJobResult, std::string>
    {
        std::shared_lock LifecycleLock(LifecycleMutex);
        if (!Impl || Impl->Stopping) return std::unexpected("Python runtime is unavailable.");
        std::lock_guard Lock(Impl->Mutex); Impl->PruneClients(); auto It = Impl->Jobs.find(Job);
        if (It == Impl->Jobs.end()) return std::unexpected("Python job is unknown or expired.");
        std::lock_guard JobLock(It->second->Mutex); return It->second->Result;
    }
    auto FPythonRuntime::CancelJob(uint64 Job) -> bool
    {
        std::shared_lock LifecycleLock(LifecycleMutex);
        if (!Impl || Impl->Stopping) return false;
        std::lock_guard Lock(Impl->Mutex); auto It = Impl->Jobs.find(Job);
        if (It == Impl->Jobs.end()) return false;
        { std::lock_guard JobLock(It->second->Mutex); if (Terminal(It->second->Result.State)) return false; }
        Impl->Cancel(It->second); return true;
    }
}
