#include "MCPServer.h"
#include "HTTPServer.h"
#include "MCPCodec.h"
#include "Tools/EditorToolSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		using namespace MCP;
		auto BearerToken(const FHTTPRequest& Request) -> std::string
		{
			const auto Value = Request.GetHeaderValue("Authorization");
			constexpr std::string_view Prefix = "Bearer ";
			if (Value.size() < Prefix.size()) return {};
			for (size_t I = 0; I < Prefix.size(); ++I)
				if (std::tolower(static_cast<unsigned char>(Value[I])) != std::tolower(static_cast<unsigned char>(Prefix[I]))) return {};
			return Value.substr(Prefix.size());
		}
		auto EqualToken(std::string_view Left, std::string_view Right) -> bool
		{
			if (Left.size() != Right.size()) return false;
			unsigned Difference = 0;
			for (size_t I = 0; I < Left.size(); ++I) Difference |= static_cast<unsigned char>(Left[I] ^ Right[I]);
			return Difference == 0;
		}
		auto Send(FHTTPResponse& Response, FToolValue Body, int Status = 200) -> void
		{
			auto TextValue = Serialize(Body);
			if (TextValue.size() > MaxResponseBytes)
			{
				TextValue = Serialize(Error(Find(Body, "id"), -32603, "Serialized response exceeds its byte budget."));
				Status = 500;
			}
			Response.SetStatus(Status);
			Response.SetContent(std::move(TextValue), "application/json");
		}

	}
	struct FMCPServer::FImpl
	{
		FHTTPServer HTTP;
		std::thread Maintenance;
		std::condition_variable Wake;
		std::shared_ptr<FEditorToolService> Service;
		FMCPServerOptions Options;
		std::atomic<bool> Stopping{false};
		std::atomic<uint32> LiveCalls{0};
		struct FPending
		{
			std::shared_ptr<FEditorToolService> Service;
			FEditorToolInvocationId Invocation;
			FToolValue RPCId;
			std::chrono::steady_clock::time_point Deadline, Heartbeat;
			// Adapter state is independent of both native execution and transport lifetime.
			enum class EState { Unadmitted, Pending, Completed, CancelRequested };
			std::atomic<EState> State{EState::Unadmitted};
			std::atomic<bool> ResponseDone{false};
			auto IsPending() const -> bool { return State == EState::Pending; }
			auto Cancel() -> bool
			{
				auto Expected = EState::Pending;
				if (!State.compare_exchange_strong(Expected, EState::CancelRequested)) return false;
				(void)Service->Cancel(Invocation); return true;
			}
			// Called by the response worker, or by maintenance only after that worker exits.
			auto Poll() -> std::optional<FToolResult>
			{
				if (!IsPending()) return {};
				auto Native = Service->GetInvocation(Invocation);
				std::optional<FToolResult> Result;
				if (!Native) Result = FToolResult{EToolStatus::Failed, {}, {Native.error()}};
				else if (Native->Result) Result = std::move(Native->Result);
				else if (Native->State == EToolInvocationState::Cancelled || Native->State == EToolInvocationState::Expired)
					Result = FToolResult{EToolStatus::Failed, {}, {{EToolError::Cancelled, "Invocation ended without a result.", {}}}};
				if (Result)
				{
					auto Expected = EState::Pending;
					if (State.compare_exchange_strong(Expected, EState::Completed)) return Result;
				}
				else if (std::chrono::steady_clock::now() >= Deadline && Cancel())
					return FToolResult{EToolStatus::Failed, {}, {{EToolError::Cancelled, "MCP call deadline exceeded; cancellation does not roll back completed effects.", {}}}};
				return {};
			}
			~FPending() { try { (void)Cancel(); } catch (...) {} }
		};
		struct FSession
		{
			bool Initialized = false;
			std::chrono::steady_clock::time_point Used;
			FToolCancellationSource Cancellation;
		};
		std::mutex SessionMutex;
		std::map<std::string, FSession, std::less<>> Sessions;
		using FCallKey = std::pair<std::string, std::string>;
		std::map<FCallKey, std::shared_ptr<FPending>> PendingCalls;
		auto CloseSessionLocked(std::string_view Id) -> void
		{
			if (auto It = Sessions.find(Id); It != Sessions.end()) It->second.Cancellation.Cancel();
			std::erase_if(PendingCalls, [&](const auto& Entry) {
				if (Entry.first.first != Id) return false;
				Entry.second->Cancel(); return true;
			});
			Sessions.erase(std::string(Id));
		}
		auto Maintain() -> void
		{
			std::unique_lock Lock(SessionMutex);
			while (!Stopping)
			{
				const auto Now = std::chrono::steady_clock::now();
				std::erase_if(PendingCalls, [&](const auto& Entry) {
					auto& P = *Entry.second;
					if (P.ResponseDone) (void)P.Poll();
					return !P.IsPending();
				});
				std::vector<std::string> Expired;
				for (const auto& [Id, Session] : Sessions) if (Now - Session.Used >= std::chrono::minutes(10)) Expired.push_back(Id);
				for (const auto& Id : Expired) CloseSessionLocked(Id);
				Wake.wait_for(Lock, std::chrono::milliseconds(20), [&] { return Stopping.load(); });
			}
		}
		std::mutex CatalogMutex;
		uint64 Revision = UINT64_MAX;
		std::shared_ptr<const MCP::FCatalog> Catalog;
		auto RequireSession(const FHTTPRequest& Wire, FHTTPResponse& Response) -> std::optional<std::string>
		{
			if (Wire.GetHeaderValueCount("Mcp-Session-Id") != 1)
			{ Send(Response, MCP::Error({}, -32600, "Expected one Mcp-Session-Id header."), 400); return {}; }
			const auto Id = Wire.GetHeaderValue("Mcp-Session-Id");
			std::lock_guard Lock(SessionMutex);
			auto It = Sessions.find(Id);
			if (It == Sessions.end()) { Send(Response, MCP::Error({}, -32600, "Unknown or expired MCP session."), 404); return {}; }
			if (Wire.GetHeaderValueCount("MCP-Protocol-Version") > 1 || (Wire.HasHeader("MCP-Protocol-Version")
				&& Wire.GetHeaderValue("MCP-Protocol-Version") != MCP::ProtocolVersion))
			{ Send(Response, MCP::Error({}, -32600, "Unsupported MCP protocol version."), 400); return {}; }
			It->second.Used = std::chrono::steady_clock::now(); return Id;
		}

		auto ReadCatalog() -> std::expected<std::shared_ptr<const MCP::FCatalog>, std::string>
		{
			auto Native = Service->GetCatalog();
			if (Native.Toolsets.empty() && std::ranges::any_of(Native.Diagnostics, [](const auto& D) { return D.Code == EToolError::ToolUnavailable; }))
				return std::unexpected("Editor tool service is closed.");
			std::lock_guard Lock(CatalogMutex);
			if (Catalog && Native.Revision < Revision) return Catalog;
			if (!Catalog || Native.Revision != Revision)
			{
				auto Built = MCP::BuildCatalog(Native);
				if (!Built) return std::unexpected(Built.error());
				Catalog = std::make_shared<const MCP::FCatalog>(std::move(*Built)); Revision = Native.Revision;
			}
			return Catalog;
		}
		auto Call(const MCP::FRequest& Request, const std::string& SessionId, FHTTPResponse& Response) -> void
		{
			using namespace MCP;
			const auto Name = Text(Find(Request.Params, "name"));
			auto Current = ReadCatalog();
			if (!Current)
			{ Send(Response, ToolResult(Request.Id, {EToolStatus::Failed, {}, {{EToolError::ToolUnavailable, Current.error(), {}}}})); return; }
			const auto It = (*Current)->find(Name);
			if (It == (*Current)->end())
			{ Send(Response, Error(Request.Id, -32602, "Unknown or unexposed tool.")); return; }
			auto Arguments = Find(Request.Params, "arguments");
			const auto& Params = std::get<FObject>(Request.Params.Data);
			if (!Params.contains("arguments")) Arguments = FObject{};
			if (!std::holds_alternative<FObject>(Arguments.Data))
			{ Send(Response, Error(Request.Id, -32602, "Tool arguments must be an object.")); return; }
			FToolRequest Native{It->second.Address, It->second.ContractVersion,
				ConvertArguments(std::move(Arguments), It->second.Input)};
			std::unique_lock SessionLock(SessionMutex);
			if (!Sessions.contains(SessionId) || Stopping) { Send(Response, Error(Request.Id, -32600, "Session is closed."), 404); return; }
			const FCallKey Key{SessionId, Serialize(Request.Id)};
			if (auto Previous = PendingCalls.find(Key); Previous != PendingCalls.end())
			{
				if (Previous->second->IsPending()) { Send(Response, Error(Request.Id, -32600, "Request id is already pending in this session."), 400); return; }
				PendingCalls.erase(Previous);
			}
			if (PendingCalls.size() >= 32) { Send(Response, Error(Request.Id, -32603, "MCP pending call limit reached."), 503); return; }
			if (LiveCalls >= 6) { Send(Response, Error(Request.Id, -32603, "MCP active call limit reached."), 503); return; }
			auto Pending = std::make_shared<FPending>();
			auto Admitted = Service->Submit(Native, EToolSource::External, {SessionId, Sessions.at(SessionId).Cancellation.GetToken()});
			if (!Admitted) { Send(Response, ToolResult(Request.Id, {EToolStatus::Failed, {}, {Admitted.error()}})); return; }
			Pending->Service = Service; Pending->Invocation = *Admitted; Pending->RPCId = Request.Id;
			Pending->State = FPending::EState::Pending;
			Pending->Heartbeat = std::chrono::steady_clock::now(); Pending->Deadline = Pending->Heartbeat + Options.CallTimeout;
			PendingCalls.emplace(Key, Pending); ++LiveCalls; SessionLock.unlock();
			Response.SetChunkedContentProvider("text/event-stream", [this, Pending](size_t Offset, FHTTPStreamSink& Sink) {
				if (Stopping || Pending->State == FPending::EState::CancelRequested) return false;
				// A transport disconnect is not a 2025-06-18 cancellation. Session
				// ownership keeps this receipt alive until completion or its deadline.
				if (!Sink.IsWritable()) return false;
				if (Offset == 0)
				{
					static constexpr std::string_view Open = ": connected\n\n";
					return Sink.Write(Open);
				}
				if (auto Result = Pending->Poll())
				{
					auto JSON = MCP::Serialize(MCP::ToolResult(Pending->RPCId, *Result));
					if (JSON.size() > MCP::MaxResponseBytes)
						JSON = MCP::Serialize(MCP::ToolResult(Pending->RPCId, {EToolStatus::Failed, {}, {{EToolError::LimitExceeded, "MCP serialized result exceeds its byte budget.", {}}}}));
					const auto Event = "event: message\ndata: " + JSON + "\n\n";
					if (!Sink.Write(Event)) return false;
					Sink.Done(); return true;
				}
				const auto Now = std::chrono::steady_clock::now();
				if (Now - Pending->Heartbeat >= std::chrono::seconds(1))
				{
					static constexpr std::string_view Heartbeat = ": pending\n\n";
					Pending->Heartbeat = Now;
					if (!Sink.Write(Heartbeat)) return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				return true;
			}, [this, Pending](bool) { Pending->ResponseDone = true; --LiveCalls; });
		}
		auto Handle(const FHTTPRequest& Wire, FHTTPResponse& Response) -> void
		{
			using namespace MCP;
			if (Stopping) { Send(Response, Error({}, -32603, "Server is stopping."), 503); return; }
			const auto ContentType = Wire.GetHeaderValue("Content-Type");
			const auto Accept = Wire.GetHeaderValue("Accept");
			if (!(ContentType == "application/json" || ContentType.starts_with("application/json;")))
			{ Send(Response, Error({}, -32600, "Expected application/json."), 415); return; }
			if (Accept.find("application/json") == std::string::npos || Accept.find("text/event-stream") == std::string::npos)
			{ Send(Response, Error({}, -32600, "Accept must include JSON and event-stream."), 406); return; }
			auto Request = ParseRequest(Wire.GetBody());
			if (!Request)
			{
				Send(Response, Error({}, static_cast<int>(Request.error().Code), Request.error().Message), 400); return;
			}
			if (Request->Method == "initialize" && !Request->bNotification)
			{
				const auto& Info = Find(Request->Params, "clientInfo");
				if (Text(Find(Request->Params, "protocolVersion")).empty() || Text(Find(Info, "name")).empty()
					|| Text(Find(Info, "version")).empty() || !std::holds_alternative<FObject>(Find(Request->Params, "capabilities").Data))
				{ Send(Response, Error(Request->Id, -32602, "Invalid initialize parameters."), 400); return; }
				std::lock_guard Lock(SessionMutex);
				if (Sessions.size() >= 16) { Send(Response, Error(Request->Id, -32603, "MCP session limit reached."), 503); return; }
				std::random_device Random;
				const auto Id = std::format("{:08x}{:08x}{:08x}{:08x}", Random(), Random(), Random(), Random());
				Sessions.emplace(Id, FSession{false, std::chrono::steady_clock::now()});
				Response.SetHeader("Mcp-Session-Id", Id);
				Send(Response, Initialize(Request->Id)); return;
			}
			auto SessionId = RequireSession(Wire, Response); if (!SessionId) return;
			if (Request->bNotification)
			{
				std::lock_guard Lock(SessionMutex);
				auto Session = Sessions.find(*SessionId);
				if (Session == Sessions.end()) { Response.SetStatus(404); return; }
				if (Request->Method == "notifications/initialized") Session->second.Initialized = true;
				else if (Request->Method == "notifications/cancelled")
				{
					const auto Key = FCallKey{*SessionId, Serialize(Find(Request->Params, "requestId"))};
					if (auto It = PendingCalls.find(Key); It != PendingCalls.end()) It->second->Cancel();
				}
				Response.SetStatus(202); return;
			}
			{
				std::lock_guard Lock(SessionMutex);
				auto It = Sessions.find(*SessionId);
				if (It == Sessions.end() || !It->second.Initialized)
				{ Send(Response, Error(Request->Id, -32600, "Session initialization is incomplete."), 400); return; }
			}
			if (Request->Method == "tools/call") { Call(*Request, *SessionId, Response); return; }
			if (Request->Method == "ping") { Send(Response, Reply(Request->Id, {})); return; }
			if (Request->Method == "tools/list")
			{
				auto Current = ReadCatalog();
				if (!Current) { Send(Response, Error(Request->Id, -32603, Current.error()), 500); return; }
				if (std::get<FObject>(Request->Params.Data).contains("cursor"))
				{ Send(Response, Error(Request->Id, -32602, "tools/list does not support cursors.")); return; }
				Send(Response, List(Request->Id, **Current));
				return;
			}
			Send(Response, Error(Request->Id, -32601, "Method not found."), 404);
		}
	};
	FMCPServer::FMCPServer() = default;
	FMCPServer::~FMCPServer() { Stop(); }
	auto FMCPServer::Start(DEditorEngine& Editor, const FMCPServerOptions& Options) -> std::expected<uint16, std::string>
	{
		if (!IsInGameThread()) return std::unexpected("MCP Start requires the GameThread.");
		if (Impl) return std::unexpected("MCP server is already started.");
		if (Options.BearerToken.size() < 32 || Options.BearerToken.size() > 128
			|| !std::ranges::all_of(Options.BearerToken, [](unsigned char C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9'); }))
			return std::unexpected("Bearer token must contain 32-128 ASCII alphanumeric characters.");
		if (Options.CallTimeout <= std::chrono::milliseconds(0) || Options.CallTimeout > std::chrono::seconds(30))
			return std::unexpected("Call timeout must be positive and at most 30 seconds.");
		auto* Tools = Editor.GetEditorSubsystem<DEditorToolSubsystem>();
		if (!Tools || !Tools->GetService()) return std::unexpected("Editor tool service is not ready.");
		(void)Tools->ListToolsets();
		auto State = std::make_unique<FImpl>();
		State->Service = Tools->GetService(); State->Options = Options;
		auto* Raw = State.get();
		auto Filter = [Raw](const FHTTPRequest& Request, FHTTPResponse& Response) {
			const auto Host = Request.GetHeaderValue("Host");
			const auto Local = "127.0.0.1:" + std::to_string(Request.GetLocalPort());
			const auto Named = "localhost:" + std::to_string(Request.GetLocalPort());
			Response.SetHeader("Cache-Control", "no-store");
			Response.SetHeader("Connection", "close");
			if (Host != Local && Host != Named) { Response.SetStatus(403); return true; }
			if (Request.GetHeaderValueCount("Host") != 1 || Request.GetHeaderValueCount("Origin") > 1)
			{ Response.SetStatus(403); return true; }
			if (Request.HasHeader("Origin"))
			{
				const auto Origin = Request.GetHeaderValue("Origin");
				if (Origin != "http://" + Local && Origin != "http://" + Named)
				{ Response.SetStatus(403); return true; }
			}
			if (Request.GetHeaderValueCount("Authorization") != 1
				|| !EqualToken(BearerToken(Request), Raw->Options.BearerToken))
			{ Response.SetStatus(401); return true; }
			return false;
		};
		std::vector<FHTTPRoute> Routes{
			{EHTTPMethod::Post, "/mcp", [Raw](const auto& Request, auto& Response) { Raw->Handle(Request, Response); }},
			{EHTTPMethod::Get, "/mcp", [Raw](const auto& Request, auto& Response) { if (Raw->RequireSession(Request, Response)) Response.SetStatus(405); }},
			{EHTTPMethod::Delete, "/mcp", [Raw](const auto& Request, auto& Response) {
				auto Id = Raw->RequireSession(Request, Response); if (!Id) return;
				std::lock_guard Lock(Raw->SessionMutex); Raw->CloseSessionLocked(*Id); Response.SetStatus(200);
			}}
		};
		FHTTPServerOptions Transport;
		Transport.Port = Options.Port;
		Transport.WorkerCount = 8; Transport.MaxQueuedConnections = 8;
		Transport.MaxBodyBytes = MCP::MaxBodyBytes;
		Transport.ReadTimeout = Transport.WriteTimeout = std::chrono::seconds(2);
		Transport.KeepAliveMaxCount = 1;
		auto Started = State->HTTP.Start(Transport, std::move(Routes), std::move(Filter), [](auto& Response, std::exception_ptr) {
			Send(Response, MCP::Error({}, -32603, "Internal MCP adapter error."), 500);
		});
		if (!Started) return std::unexpected("Could not start the local MCP HTTP server: " + Started.error());
		Impl = std::move(State);
		Impl->Maintenance = std::thread([Raw] { Raw->Maintain(); });
		return *Started;
	}
	auto FMCPServer::Stop() noexcept -> void
	{
		if (!Impl) return;
		Impl->Stopping = true;
		{
			std::lock_guard Lock(Impl->SessionMutex);
			for (auto& [Key, Pending] : Impl->PendingCalls) Pending->Cancel();
			Impl->PendingCalls.clear();
			while (!Impl->Sessions.empty()) Impl->CloseSessionLocked(Impl->Sessions.begin()->first);
		}
		Impl->Wake.notify_all();
		Impl->HTTP.Stop();
		if (Impl->Maintenance.joinable()) Impl->Maintenance.join();
		Impl.reset();
	}
}
