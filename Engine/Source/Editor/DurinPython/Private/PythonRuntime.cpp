#include "PythonRuntime.h"
#include "Tools/EditorToolService.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#if DURIN_WITH_PYTHON
// The packaged interpreter uses the release ABI in every editor configuration.
#pragma push_macro("_DEBUG")
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#pragma pop_macro("_DEBUG")
#include <Windows.h>
#include "PythonABI.h"
#endif

namespace Durin
{
    namespace
    {
        constexpr size_t MaxSource = 1024 * 1024;
        constexpr size_t MaxOutput = 64 * 1024;
        std::atomic<uint64> NextIdentity{1};
        std::atomic<bool> InterpreterOwned{false};
        auto Terminal(EPythonJobState State) -> bool
        { return State != EPythonJobState::Queued && State != EPythonJobState::Running; }

        // Owned job data is independent of editor/provider objects.
        struct FJob
        {
            uint64 Id = 0, Session = 0;
            EToolSource Source = EToolSource::External;
            std::shared_ptr<FEditorToolService> Endpoint;
            std::string Code, Filename;
            std::atomic<bool> Cancelled{false};
            std::mutex Mutex;
            FPythonJobResult Result;
            bool OutputOverflow = false;
            std::chrono::steady_clock::time_point Deadline;
            std::map<uint64, FEditorToolInvocationId> Invocations;
        };
    }

#if DURIN_WITH_PYTHON
    namespace
    {
        thread_local FJob* CurrentJob = nullptr;
        PyObject* ToolError = nullptr;
        PyObject* CancelledError = nullptr;

        struct FPyDelete { auto operator()(PyObject* Object) const -> void { Py_XDECREF(Object); } };
        using FPy = std::unique_ptr<PyObject, FPyDelete>;
        auto Fail(const char* Message) -> PyObject* { PyErr_SetString(PyExc_ValueError, Message); return nullptr; }
        auto CheckCancelled() -> bool
        {
            if (!CurrentJob) { PyErr_SetString(PyExc_RuntimeError, "No active Durin script job."); return true; }
            if (CurrentJob->Cancelled || std::chrono::steady_clock::now() >= CurrentJob->Deadline)
            {
                CurrentJob->Cancelled = true;
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
        auto RequestImpl(PyObject*, PyObject* Args) -> PyObject*
        {
            if (CheckCancelled()) return nullptr;
            const char* Operation; PyObject* Params;
            if (!PyArg_ParseTuple(Args, "sO!", &Operation, &PyTuple_Type, &Params)) return nullptr;
            const std::string_view Op(Operation);
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
                CurrentJob->Result.Output.append(Text, std::min(Available, static_cast<size_t>(Size)));
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
            if (Op == "describe" || Op == "submit")
            {
                const char* Set; const char* Name; PyObject* Input = nullptr; PyObject* Version = Py_None;
                if (Op == "describe") { if (!PyArg_ParseTuple(Params, "ss", &Set, &Name)) return nullptr; }
                else if (!PyArg_ParseTuple(Params, "ssOO", &Set, &Name, &Input, &Version)) return nullptr;
                auto Descriptor = FindTool(Set, Name); if (!Descriptor) return nullptr;
                if (Op == "describe") return ToPython(FToolValue::FObject{{"toolset", Set}, {"tool", Name},
                    {"version", static_cast<uint64>(Descriptor->ContractVersion)}, {"description", Descriptor->Description},
                    {"input", SchemaValue(Descriptor->Input)}, {"output", SchemaValue(Descriptor->Output)}});
                if (CurrentJob->Invocations.size() >= 256) return Fail("Script invocation budget exceeded.");
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
                auto Submitted = CurrentJob->Endpoint->Submit({{Set, Name}, Contract, std::move(*Value)}, CurrentJob->Source);
                if (!Submitted) return RaiseTool({Submitted.error()});
                const uint64 Id = NextIdentity++;
                CurrentJob->Invocations.emplace(Id, *Submitted);
                return Py_BuildValue("KK", static_cast<unsigned long long>(CurrentJob->Id), static_cast<unsigned long long>(Id));
            }
            if (Op == "state" || Op == "cancel" || Op == "result")
            {
                unsigned long long Owner, Id; double Timeout = 30;
                if (!PyArg_ParseTuple(Params, "KK|d", &Owner, &Id, &Timeout)) return nullptr;
                if (Owner != CurrentJob->Id) return Fail("Invocation belongs to another script job/session.");
                auto It = CurrentJob->Invocations.find(Id);
                if (It == CurrentJob->Invocations.end()) return Fail("Unknown invocation handle.");
                if (!std::isfinite(Timeout) || Timeout < 0 || Timeout > 30) return Fail("Timeout must be between 0 and 30 seconds.");
                if (Op == "cancel")
                {
                    auto Result = CurrentJob->Endpoint->Cancel(It->second);
                    if (!Result) return RaiseTool({Result.error()});
                    Py_RETURN_NONE;
                }
                std::expected<FEditorToolInvocation, FToolDiagnostic> Result = std::unexpected(FToolDiagnostic{});
                const auto Deadline = std::min(CurrentJob->Deadline, std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(Timeout)));
                bool TimedOut = false;
                PyThreadState* Thread = PyEval_SaveThread();
                try
                {
                    do
                    {
                        Result = CurrentJob->Endpoint->GetInvocation(It->second);
                        if (!Result || Result->Result || Op == "state" || CurrentJob->Cancelled) break;
                        if (std::chrono::steady_clock::now() >= Deadline) { TimedOut = true; break; }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    } while (true);
                    if (CurrentJob->Cancelled || TimedOut) (void)CurrentJob->Endpoint->Cancel(It->second);
                }
                catch (...) { PyEval_RestoreThread(Thread); throw; }
                PyEval_RestoreThread(Thread);
                if (CheckCancelled()) return nullptr;
                if (TimedOut) { PyErr_SetString(PyExc_TimeoutError, "Tool wait timed out; cancellation requested without rollback guarantee."); return nullptr; }
                if (!Result) return RaiseTool({Result.error()});
                if (Op == "state")
                {
                    const char* Names[] = {"queued", "running", "completed", "cancelled", "expired"};
                    return PyUnicode_FromString(Names[static_cast<size_t>(Result->State)]);
                }
                if (!Result->Result) { PyErr_SetString(ToolError, "Tool receipt has no final result."); return nullptr; }
                if (Result->Result->Status != EToolStatus::Succeeded) return RaiseTool(Result->Result->Diagnostics);
                return ToPython(Result->Result->Data);
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
        PyModuleDef Module = {PyModuleDef_HEAD_INIT, "durin", nullptr, -1, Methods};
        auto InitModule() -> PyObject*
        {
            FPy Result(PyModule_Create(&Module)); if (!Result) return nullptr;
            ToolError = PyErr_NewException("durin.ToolError", PyExc_RuntimeError, nullptr);
            CancelledError = PyErr_NewException("durin.CancelledError", PyExc_BaseException, nullptr);
            if (!ToolError || !CancelledError) return nullptr;
            if (PyModule_AddObjectRef(Result.get(), "ToolError", ToolError) < 0 ||
                PyModule_AddObjectRef(Result.get(), "CancelledError", CancelledError) < 0) return nullptr;
            return Result.release();
        }
        constexpr const char* Bootstrap = R"PY(
class _Invocation:
    def __init__(self, identity): self._identity = identity
    def state(self): return _request('state', self._identity)
    def result(self, timeout=30.0): return _request('result', (*self._identity, timeout))
    def cancel(self): return _request('cancel', self._identity)
class _Tools:
    def __init__(self, owner): self._owner = owner
    def _check(self): _request('owner', (self._owner,))
    def list(self):
        self._check()
        return _request('list', ())
    def describe(self, toolset, tool):
        self._check()
        return _request('describe', (toolset, tool))
    def submit(self, toolset, tool, arguments, *, version=None):
        self._check()
        return _Invocation(_request('submit', (toolset, tool, arguments, version)))
    def call(self, toolset, tool, arguments, *, version=None, timeout=30.0):
        return self.submit(toolset, tool, arguments, version=version).result(timeout)
class _Output:
    def write(self, text): return _request('write', (text,))
    def flush(self): pass
tools = _Tools(0)
)PY";
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
            PyErr_Clear(); if (Result.size() > MaxOutput) Result.resize(MaxOutput); return Result;
        }
    }
#endif

    struct FPythonRuntime::FImpl
    {
        struct FSession { std::shared_ptr<FEditorToolService> Endpoint; EToolSource Source; };
        mutable std::mutex Mutex;
        std::condition_variable Wake;
        std::map<uint64, FSession> Sessions;
        std::map<uint64, std::shared_ptr<FJob>> Jobs;
        std::deque<std::shared_ptr<FJob>> Queue;
        std::thread Worker;
        bool Stopping = false;
        std::filesystem::path Directory;

        auto Cancel(const std::shared_ptr<FJob>& Job) -> void
        {
            Job->Cancelled = true;
            // The worker owns the invocation map; its interruptible wait performs native cancellation.
            std::lock_guard Lock(Job->Mutex);
            if (Job->Result.State == EPythonJobState::Queued)
            {
                Job->Result.State = EPythonJobState::Cancelled; Job->Endpoint.reset();
                std::erase(Queue, Job); Job->Code.clear();
            }
        }
#if DURIN_WITH_PYTHON
        auto Execute(FJob& Job) -> void
        {
            CurrentJob = &Job; Job.Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            FPy Globals(PyDict_New()); FPy Name(PyUnicode_FromString("__main__"));
            FPy File(PyUnicode_DecodeUTF8(Job.Filename.data(), static_cast<Py_ssize_t>(Job.Filename.size()), "strict"));
            FPy Sys(PyImport_ImportModule("sys")); FPy Durin(PyImport_ImportModule("durin"));
            FPy Tools(Durin ? PyObject_CallMethod(Durin.get(), "_Tools", "K", static_cast<unsigned long long>(Job.Id)) : nullptr);
            FPy Output(Durin ? PyObject_CallMethod(Durin.get(), "_Output", nullptr) : nullptr);
            FPy OldOut(Sys ? PyObject_GetAttrString(Sys.get(), "stdout") : nullptr);
            FPy OldErr(Sys ? PyObject_GetAttrString(Sys.get(), "stderr") : nullptr);
            bool Ready = Globals && Name && File && Sys && Tools && Output && OldOut && OldErr;
            if (Ready) Ready = PyDict_SetItemString(Globals.get(), "__name__", Name.get()) == 0 &&
                PyDict_SetItemString(Globals.get(), "__file__", File.get()) == 0 &&
                PyObject_SetAttrString(Durin.get(), "tools", Tools.get()) == 0 &&
                PyObject_SetAttrString(Sys.get(), "stdout", Output.get()) == 0 && PyObject_SetAttrString(Sys.get(), "stderr", Output.get()) == 0;
            FPy Source(Ready ? PyUnicode_DecodeUTF8(Job.Code.data(), static_cast<Py_ssize_t>(Job.Code.size()), "strict") : nullptr);
            FPy Code(Source ? Py_CompileString(Job.Code.c_str(), Job.Filename.c_str(), Py_file_input) : nullptr);
            PyEval_SetTrace(Trace, nullptr);
            FPy Result(Code ? PyEval_EvalCode(Code.get(), Globals.get(), Globals.get()) : nullptr);
            PyEval_SetTrace(nullptr, nullptr);
            std::string Error = Result ? "" : PythonError();
            if (Sys && OldOut) (void)PyObject_SetAttrString(Sys.get(), "stdout", OldOut.get());
            if (Sys && OldErr) (void)PyObject_SetAttrString(Sys.get(), "stderr", OldErr.get());
            PyErr_Clear();
            for (const auto& [Id, Invocation] : Job.Invocations)
            {
                (void)Id;
                auto Receipt = Job.Endpoint->GetInvocation(Invocation);
                if (Receipt && !Receipt->Result) (void)Job.Endpoint->Cancel(Invocation);
            }
            {
                std::lock_guard Lock(Job.Mutex);
                Job.Result.State = Job.Cancelled ? EPythonJobState::Cancelled : Result && !Job.OutputOverflow ? EPythonJobState::Succeeded : EPythonJobState::Failed;
                Job.Result.Error = Job.OutputOverflow ? "Script output exceeded 64 KiB." : std::move(Error);
            }
            CurrentJob = nullptr;
            Job.Endpoint.reset(); Job.Invocations.clear(); Job.Code.clear();
        }
        auto Run(std::promise<std::string> Initialized) -> void
        {
            static std::once_flag Registration;
            std::call_once(Registration, [] { PyImport_AppendInittab("durin", InitModule); });
            PyConfig Config; PyConfig_InitIsolatedConfig(&Config);
            Config.site_import = 0; Config.write_bytecode = 0; Config.install_signal_handlers = 0;
            Config.parse_argv = 0; Config.module_search_paths_set = 1;
            auto Zip = (Directory / "python313.zip").wstring();
            PyStatus Status = PyWideStringList_Append(&Config.module_search_paths, Zip.c_str());
            if (!PyStatus_Exception(Status)) Status = Py_InitializeFromConfig(&Config);
            PyConfig_Clear(&Config);
            if (PyStatus_Exception(Status))
            { Initialized.set_value(Status.err_msg ? Status.err_msg : "CPython initialization failed."); return; }
            FPy Durin(PyImport_ImportModule("durin"));
            FPy Boot(Durin ? PyRun_String(Bootstrap, Py_file_input, PyModule_GetDict(Durin.get()), PyModule_GetDict(Durin.get())) : nullptr);
            if (!Boot)
            {
                Initialized.set_value(PythonError()); Boot.reset(); Durin.reset();
                Py_CLEAR(ToolError); Py_CLEAR(CancelledError); Py_FinalizeEx(); return;
            }
            Boot.reset(); Durin.reset(); Initialized.set_value("");
            for (;;)
            {
                PyThreadState* Thread = PyEval_SaveThread();
                std::shared_ptr<FJob> Job;
                {
                    std::unique_lock Lock(Mutex); Wake.wait(Lock, [&] { return Stopping || !Queue.empty(); });
                    if (!Queue.empty()) { Job = Queue.front(); Queue.pop_front(); }
                }
                PyEval_RestoreThread(Thread);
                if (!Job) break;
                {
                    std::lock_guard Lock(Job->Mutex);
                    if (Job->Result.State == EPythonJobState::Cancelled) continue;
                    Job->Result.State = EPythonJobState::Running;
                }
                try { Execute(*Job); }
                catch (const std::exception& Error)
                {
                    PyEval_SetTrace(nullptr, nullptr); PyErr_Clear(); CurrentJob = nullptr;
                    for (const auto& [Id, Invocation] : Job->Invocations) { (void)Id; (void)Job->Endpoint->Cancel(Invocation); }
                    Job->Endpoint.reset(); Job->Code.clear();
                    std::lock_guard Lock(Job->Mutex); Job->Result.State = EPythonJobState::Failed; Job->Result.Error = Error.what();
                }
            }
            Py_CLEAR(ToolError); Py_CLEAR(CancelledError);
            Py_FinalizeEx();
        }
#endif
    };

    FPythonRuntime::FPythonRuntime() = default;
    FPythonRuntime::~FPythonRuntime() { Stop(); }
    auto FPythonRuntime::Start(const FPythonRuntimeOptions& Options) -> std::expected<void, std::string>
    {
        if (Impl) return std::unexpected("Python runtime already started.");
#if DURIN_WITH_PYTHON
        bool Expected = false;
        if (!InterpreterOwned.compare_exchange_strong(Expected, true)) return std::unexpected("Another Durin Python runtime owns the interpreter.");
        auto Directory = Options.RuntimeDirectory;
        if (Directory.empty())
        {
            Directory = std::filesystem::path(FPlatformProcess::ExecutablePath()).parent_path();
            if (!std::filesystem::exists(Directory / "python313.zip")) Directory = FPaths::EngineThirdPartyRuntimeBinariesDir();
        }
        if (!std::filesystem::exists(Directory / "python313.zip") || !std::filesystem::exists(Directory / "python313.dll"))
        { InterpreterOwned = false; return std::unexpected("Packaged Python runtime is missing; prepare python and rebuild DurinPython."); }
        // Resolve only the qualified DLL; retain it for process lifetime.
        const auto Library = LoadLibraryExW((Directory / "python313.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!Library || !PythonABI::API.Load(Library))
        { InterpreterOwned = false; return std::unexpected("Could not load packaged python313.dll."); }
        if (Py_IsInitialized())
        { InterpreterOwned = false; return std::unexpected("An existing Python interpreter is not owned by Durin."); }
        Impl = std::make_unique<FImpl>(); Impl->Directory = std::move(Directory);
        std::promise<std::string> Initialized; auto Ready = Initialized.get_future();
        Impl->Worker = std::thread([this, Promise = std::move(Initialized)]() mutable { Impl->Run(std::move(Promise)); });
        auto Error = Ready.get();
        if (!Error.empty()) { Stop(); return std::unexpected(std::move(Error)); }
        return {};
#else
        (void)Options;
        return std::unexpected("Python embedding is unavailable on this host or the qualified SDK was not prepared.");
#endif
    }
    auto FPythonRuntime::Stop() noexcept -> void
    {
        if (!Impl) return;
        {
            std::lock_guard Lock(Impl->Mutex); Impl->Stopping = true; Impl->Sessions.clear();
            for (const auto& [Id, Job] : Impl->Jobs) { (void)Id; Impl->Cancel(Job); }
        }
        Impl->Wake.notify_all();
        if (Impl->Worker.joinable()) Impl->Worker.join();
        Impl.reset(); InterpreterOwned = false;
    }
    auto FPythonRuntime::CreateSession(std::shared_ptr<FEditorToolService> Endpoint, EToolSource Source) -> std::expected<uint64, std::string>
    {
        if (!Impl || !Endpoint) return std::unexpected("Python runtime or editor endpoint is unavailable.");
        std::lock_guard Lock(Impl->Mutex);
        if (Impl->Stopping || Impl->Sessions.size() >= 8) return std::unexpected("Python session admission is closed or full.");
        const uint64 Id = NextIdentity++; Impl->Sessions.emplace(Id, FImpl::FSession{std::move(Endpoint), Source}); return Id;
    }
    auto FPythonRuntime::CloseSession(uint64 Session) -> void
    {
        if (!Impl) return;
        std::lock_guard Lock(Impl->Mutex); Impl->Sessions.erase(Session);
        for (const auto& [Id, Job] : Impl->Jobs) { (void)Id; if (Job->Session == Session) Impl->Cancel(Job); }
        Impl->Wake.notify_all();
    }
    auto FPythonRuntime::RunString(uint64 Session, std::string Code, std::string Filename) -> std::expected<uint64, std::string>
    {
        if (!Impl) return std::unexpected("Python runtime is unavailable.");
        if (Code.size() > MaxSource || Code.find('\0') != std::string::npos || Filename.size() > 4096 || Filename.find('\0') != std::string::npos)
            return std::unexpected("Script source/filename exceeds limits or contains NUL.");
        std::lock_guard Lock(Impl->Mutex);
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
        Job->Endpoint = It->second.Endpoint; Job->Source = It->second.Source;
        Job->Code = std::move(Code); Job->Filename = std::move(Filename);
        Impl->Jobs.emplace(Job->Id, Job); Impl->Queue.push_back(Job); Impl->Wake.notify_one(); return Job->Id;
    }
    auto FPythonRuntime::RunFile(uint64 Session, const std::filesystem::path& Path) -> std::expected<uint64, std::string>
    {
        if (!Path.is_absolute()) return std::unexpected("Script path must be absolute.");
        std::ifstream Stream(Path, std::ios::binary);
        if (!Stream) return std::unexpected("Could not open Python script.");
        std::string Source(MaxSource + 1, '\0'); Stream.read(Source.data(), static_cast<std::streamsize>(Source.size()));
        const auto Count = Stream.gcount();
        if (Stream.bad() || Count > static_cast<std::streamsize>(MaxSource)) return std::unexpected("Script read failed or source exceeds 1 MiB.");
        Source.resize(static_cast<size_t>(Count));
        const auto Filename = Path.u8string();
        return RunString(Session, std::move(Source), std::string(reinterpret_cast<const char*>(Filename.data()), Filename.size()));
    }
    auto FPythonRuntime::GetJob(uint64 Job) const -> std::expected<FPythonJobResult, std::string>
    {
        if (!Impl) return std::unexpected("Python runtime is unavailable.");
        std::lock_guard Lock(Impl->Mutex); auto It = Impl->Jobs.find(Job);
        if (It == Impl->Jobs.end()) return std::unexpected("Python job is unknown or expired.");
        std::lock_guard JobLock(It->second->Mutex); return It->second->Result;
    }
    auto FPythonRuntime::CancelJob(uint64 Job) -> bool
    {
        if (!Impl) return false;
        std::lock_guard Lock(Impl->Mutex); auto It = Impl->Jobs.find(Job);
        if (It == Impl->Jobs.end()) return false;
        { std::lock_guard JobLock(It->second->Mutex); if (Terminal(It->second->Result.State)) return false; }
        Impl->Cancel(It->second); Impl->Wake.notify_all(); return true;
    }
}
