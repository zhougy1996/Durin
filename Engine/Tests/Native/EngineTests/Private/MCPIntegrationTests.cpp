#include <httplib.h>
#include "MCPServer.h"
#include "MCPCodec.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngine.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include <gtest/gtest.h>

namespace Durin
{
	class FMCPTestEditor : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FMCPTestEditor, DEditorEngine, FMCPTestEditor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FMCPTestEditor)
	public:
		explicit FMCPTestEditor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Start() -> FSubsystemResult { return ActivateEditor() ? InitializeEditorSubsystems() : FSubsystemResult{ESubsystemError::InvalidState, "Editor activation failed."}; }
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FMCPTestEditor)
	struct FEditorEngineTestAccess
	{
		static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void { Editor.EditorWorld = World; Editor.SetWorld(World); }
	};
}
namespace
{
	using namespace Durin;
	using namespace Durin::MCP;
	constexpr std::string_view Token = "DurinMCPIntegrationTestToken123456789";
	std::string TestSession;
	auto InitializeParams() -> FObject
	{
		return {{"protocolVersion", ProtocolVersion}, {"capabilities", FObject{}},
			{"clientInfo", FObject{{"name", "native-test"}, {"version", "1"}}}};
	}
	auto Body(std::string Method, FObject Params = {}) -> std::string
	{
		return Serialize(FObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", std::move(Method)}, {"params", std::move(Params)}});
	}
	auto Headers() -> httplib::Headers
	{
		httplib::Headers Result{{"Authorization", "Bearer " + std::string(Token)}, {"Accept", "application/json, text/event-stream"},
			{"MCP-Protocol-Version", std::string(ProtocolVersion)}};
		if (!TestSession.empty()) Result.emplace("Mcp-Session-Id", TestSession);
		return Result;
	}
	class FMCPIntegrationTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			Host = TStrongObjectPtr<FMCPTestEditor>(NewObject<FMCPTestEditor>(nullptr, {}));
			ASSERT_TRUE(Host->Start());
			ASSERT_TRUE(FModuleManager::Get().LoadModule("LevelEditor"));
			ASSERT_TRUE(Host->RefreshEditorSubsystems()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
			auto Started = Server.Start(*Host, {.BearerToken = std::string(Token)});
			ASSERT_TRUE(Started) << Started.error(); Port = *Started;
			Connect();
		}
		auto TearDown() -> void override
		{
			Server.Stop(); TestSession.clear(); Registration.Reset(); Toolset.reset();
			Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage();
		}
		auto Connect() -> void
		{
			httplib::Client Client("127.0.0.1", Port); auto Auth = Headers(); Auth.erase("Mcp-Session-Id");
			auto Result = Client.Post("/mcp", Auth, Body("initialize", InitializeParams()), "application/json");
			ASSERT_TRUE(Result); ASSERT_EQ(200, Result->status);
			TestSession = Result->get_header_value("Mcp-Session-Id"); ASSERT_FALSE(TestSession.empty());
			ASSERT_EQ(202, Notify("notifications/initialized")->status);
		}
		auto Notify(std::string Method, FObject Params = {}) -> httplib::Result
		{
			httplib::Client Client("127.0.0.1", Port);
			return Client.Post("/mcp", Headers(), Serialize(FObject{{"jsonrpc", "2.0"}, {"method", std::move(Method)}, {"params", std::move(Params)}}), "application/json");
		}
		auto Post(std::string Method = "tools/list", FObject Params = {}, std::optional<httplib::Headers> Custom = {}) -> httplib::Result
		{
			httplib::Client Client("127.0.0.1", Port); Client.set_read_timeout(3, 0);
			return Client.Post("/mcp", Custom.value_or(Headers()), Body(Method, std::move(Params)), "application/json");
		}
		auto Register(std::function<FToolResult(const FToolRequest&, const FToolContext&)> Handler = {}) -> void
		{
			FToolDescriptor Descriptor{.Name = "echo", .Output = FToolSchema(EToolValueType::Boolean), .bExposeExternally = true};
			FToolBinding Binding{Descriptor};
			Binding.Handler = [this, Handler](const auto& Request, const auto& Context) {
				++Calls; EXPECT_EQ(EToolSource::External, Context.Source); EXPECT_EQ(Host.Get(), GetToolEditor(Context));
				return Handler ? Handler(Request, Context) : FToolResult{EToolStatus::Succeeded, true};
			};
			Toolset = std::make_unique<FNativeToolset>("tests", "MCP test tools", std::vector<FToolBinding>{std::move(Binding)});
			Registration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset);
			(void)Tools->ListToolsets();
		}
		auto BeginCall(std::function<void()> OnHeaders = {}) -> std::future<httplib::Result>
		{
			auto CallHeaders = Headers();
			return std::async(std::launch::async, [this, OnHeaders, CallHeaders] {
				httplib::Client Client("127.0.0.1", Port); Client.set_read_timeout(3, 0);
				httplib::Request Request; Request.method = "POST"; Request.path = "/mcp";
				Request.headers = CallHeaders; Request.set_header("Content-Type", "application/json");
				Request.body = Body("tools/call", {{"name", "tests.echo"}});
				Request.response_handler = [OnHeaders](const auto&) { if (OnHeaders) OnHeaders(); return true; };
				return Client.send(Request);
			});
		}
		auto Pump(std::future<httplib::Result>& Future) -> httplib::Result
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now() < Deadline)
			{ Tools->TickTools(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			return Future.get();
		}
		static auto Wait(const std::atomic<bool>& Flag) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!Flag && std::chrono::steady_clock::now() < Deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return Flag;
		}
		FMCPServer Server;
		uint16 Port = 0;
		TStrongObjectPtr<FMCPTestEditor> Host;
		DEditorToolSubsystem* Tools = nullptr;
		std::unique_ptr<FNativeToolset> Toolset;
		FModularFeatureRegistration Registration;
		int Calls = 0;
	};
	TEST_F(FMCPIntegrationTests, DiscoversRealPIEToolsAndDeclaresOnlySupportedCapabilities)
	{
		auto Discovered = Post("initialize", InitializeParams()); ASSERT_TRUE(Discovered); EXPECT_EQ(200, Discovered->status);
		FJsonDocument Document; ASSERT_TRUE(Document.Parse(Discovered->body));
		auto Capabilities = Document.GetRootView().GetView("result").GetView("capabilities");
		EXPECT_TRUE(Capabilities.Contains("tools")); EXPECT_FALSE(Capabilities.Contains("resources")); EXPECT_FALSE(Capabilities.Contains("prompts"));
		auto Listed = Post(); ASSERT_TRUE(Listed); EXPECT_EQ(200, Listed->status);
		EXPECT_NE(std::string::npos, Listed->body.find("editor.pie.pause"));
		EXPECT_EQ(std::string::npos, Listed->body.find("toggle_pause"));
		auto Ping = Post("ping"); ASSERT_TRUE(Ping); EXPECT_EQ(200, Ping->status);
		EXPECT_EQ("application/json", Ping->get_header_value("Content-Type"));
		EXPECT_EQ(Serialize(Reply(1, {})), Ping->body);
		auto Cursor = Post("tools/list", {{"cursor", "old-cursor"}}); ASSERT_TRUE(Cursor);
		EXPECT_NE(std::string::npos, Cursor->body.find("-32602"));
	}
	TEST_F(FMCPIntegrationTests, RejectsInvalidAuthenticationOriginHostAndHeaders)
	{
		auto Missing = Headers(); Missing.erase("Authorization"); EXPECT_EQ(401, Post("tools/list", {}, Missing)->status);
		auto Lowercase = Headers(); Lowercase.erase("Authorization"); Lowercase.emplace("Authorization", "bearer " + std::string(Token));
		EXPECT_EQ(200, Post("tools/list", {}, Lowercase)->status);
		auto Origin = Headers(); Origin.emplace("Origin", "https://untrusted.example"); EXPECT_EQ(403, Post("tools/list", {}, Origin)->status);
		auto HostHeader = Headers(); HostHeader.emplace("Host", "untrusted.example"); EXPECT_EQ(403, Post("tools/list", {}, HostHeader)->status);
		auto Mismatch = Headers(); Mismatch.erase("MCP-Protocol-Version"); Mismatch.emplace("MCP-Protocol-Version", "2026-07-28");
		EXPECT_EQ(400, Post("tools/list", {}, Mismatch)->status);
		auto Allowed = Headers(); Allowed.emplace("Origin", "http://127.0.0.1:" + std::to_string(Port)); EXPECT_EQ(200, Post("tools/list", {}, Allowed)->status);
	}
	TEST_F(FMCPIntegrationTests, RejectsMalformedBodiesOversizeBodiesAndUnsupportedMethods)
	{
		httplib::Client Client("127.0.0.1", Port);
		auto Malformed = Client.Post("/mcp", Headers(), "{", "application/json"); ASSERT_TRUE(Malformed); EXPECT_EQ(400, Malformed->status);
		EXPECT_NE(std::string::npos, Malformed->body.find("-32700"));
		auto Invalid = Client.Post("/mcp", Headers(), "[]", "application/json"); ASSERT_TRUE(Invalid);
		EXPECT_EQ(400, Invalid->status); EXPECT_NE(std::string::npos, Invalid->body.find("-32600"));
		auto Huge = Client.Post("/mcp", Headers(), std::string(MaxBodyBytes + 1, ' '), "application/json"); ASSERT_TRUE(Huge); EXPECT_EQ(413, Huge->status);
		EXPECT_EQ(404, Post("resources/list")->status);
		EXPECT_EQ(405, Client.Get("/mcp", Headers())->status);
		EXPECT_EQ(200, Client.Delete("/mcp", Headers())->status);
		EXPECT_EQ(404, Client.Get("/mcp", Headers())->status);
	}
	TEST_F(FMCPIntegrationTests, RejectsBadOptionsAndOccupiedPortsAndRestartsCleanly)
	{
		FMCPServer Other;
		auto Occupied = Other.Start(*Host, {.Port = Port, .BearerToken = std::string(Token)});
		ASSERT_FALSE(Occupied);
		EXPECT_NE(std::string::npos, Occupied.error().find("Could not bind the HTTP port."));
		EXPECT_FALSE(Other.Start(*Host, {.BearerToken = "short"}));
		EXPECT_FALSE(Other.Start(*Host, {.BearerToken = std::string(Token), .CallTimeout = std::chrono::milliseconds(0)}));
		Server.Stop(); Server.Stop();
		auto Restarted = Server.Start(*Host, {.Port = Port, .BearerToken = std::string(Token)}); ASSERT_TRUE(Restarted);
		Connect();
		EXPECT_EQ(200, Post()->status);
	}
	TEST_F(FMCPIntegrationTests, StopsWhilePeerHasOnlySentPartOfTheRequestBody)
	{
		const auto Socket = ::socket(AF_INET, SOCK_STREAM, 0); ASSERT_NE(INVALID_SOCKET, Socket);
		sockaddr_in Address{}; Address.sin_family = AF_INET; Address.sin_port = htons(Port); Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(0, ::connect(Socket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)));
		const std::string Partial = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(Port)
			+ "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{";
		ASSERT_GT(::send(Socket, Partial.data(), static_cast<int>(Partial.size()), 0), 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		const auto Begin = std::chrono::steady_clock::now(); Server.Stop();
		EXPECT_LT(std::chrono::steady_clock::now() - Begin, std::chrono::seconds(3));
		httplib::detail::close_socket(Socket);
	}
	TEST_F(FMCPIntegrationTests, CallsThroughGameThreadAndKeepsRepeatedClientIdsIndependent)
	{
		Register(); auto First = BeginCall(); Connect(); auto Second = BeginCall();
		auto A = Pump(First); auto B = Pump(Second); ASSERT_TRUE(A); ASSERT_TRUE(B);
		EXPECT_EQ(2, Calls); EXPECT_NE(std::string::npos, A->body.find("\"data\":true"));
		EXPECT_NE(std::string::npos, B->body.find("\"isError\":false"));
		EXPECT_EQ("text/event-stream", A->get_header_value("Content-Type"));
	}
	TEST_F(FMCPIntegrationTests, CancelsQueuedCallWithAnExplicitSessionNotification)
	{
		Register();
		httplib::Client Client("127.0.0.1", Port);
		httplib::Request Request; Request.method = "POST"; Request.path = "/mcp";
		Request.headers = Headers(); Request.set_header("Content-Type", "application/json");
		Request.body = Body("tools/call", {{"name", "tests.echo"}});
		std::atomic<bool> Opened = false;
		Request.content_receiver = [&](const char*, size_t, uint64_t, uint64_t) { Opened = true; return false; };
		(void)Client.send(Request); ASSERT_TRUE(Opened);
		ASSERT_EQ(202, Notify("notifications/cancelled", {{"requestId", 1}})->status);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool Idle = false;
		// Configure succeeds only after the service has no queued/running work;
		// this observes cancellation without guessing private receipt identities.
		while (!(Idle = Tools->GetService()->Configure(FEditorToolLimits{}).has_value()) && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		EXPECT_TRUE(Idle); Tools->TickTools(); EXPECT_EQ(0, Calls);
	}
	TEST_F(FMCPIntegrationTests, DisconnectAloneDoesNotCancelAdmittedWork)
	{
		Register(); httplib::Client Client("127.0.0.1", Port); httplib::Request Request;
		Request.method = "POST"; Request.path = "/mcp"; Request.headers = Headers();
		Request.set_header("Content-Type", "application/json"); Request.body = Body("tools/call", {{"name", "tests.echo"}});
		std::atomic<bool> Opened = false;
		Request.content_receiver = [&](const char*, size_t, uint64_t, uint64_t) { Opened = true; return false; };
		(void)Client.send(Request); ASSERT_TRUE(Opened);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		EXPECT_FALSE(Tools->GetService()->Configure(FEditorToolLimits{}));
		Tools->TickTools(); EXPECT_EQ(1, Calls);
	}
	TEST_F(FMCPIntegrationTests, DisconnectedQueuedCallStillExpiresAtItsDeadline)
	{
		Register(); Server.Stop();
		auto Started = Server.Start(*Host, {.BearerToken = std::string(Token), .CallTimeout = std::chrono::milliseconds(200)});
		ASSERT_TRUE(Started); Port = *Started; Connect();
		httplib::Client Client("127.0.0.1", Port); httplib::Request Request;
		Request.method = "POST"; Request.path = "/mcp"; Request.headers = Headers();
		Request.set_header("Content-Type", "application/json"); Request.body = Body("tools/call", {{"name", "tests.echo"}});
		bool Opened = false;
		Request.content_receiver = [&](const char*, size_t, uint64_t, uint64_t) { Opened = true; return false; };
		(void)Client.send(Request); ASSERT_TRUE(Opened);
		EXPECT_FALSE(Tools->GetService()->Configure(FEditorToolLimits{}));
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool Idle = false;
		while (!(Idle = Tools->GetService()->Configure(FEditorToolLimits{}).has_value()) && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		EXPECT_TRUE(Idle); Tools->TickTools(); EXPECT_EQ(0, Calls);
	}
	TEST_F(FMCPIntegrationTests, CancellationCannotCrossSessionBoundaries)
	{
		Register(); std::atomic<bool> Opened = false;
		auto First = BeginCall([&] { Opened = true; }); ASSERT_TRUE(Wait(Opened));
		Connect(); ASSERT_EQ(202, Notify("notifications/cancelled", {{"requestId", 1}})->status);
		EXPECT_FALSE(Tools->GetService()->Configure(FEditorToolLimits{}));
		auto Response = Pump(First); ASSERT_TRUE(Response); EXPECT_EQ(1, Calls);
		EXPECT_NE(std::string::npos, Response->body.find("\"isError\":false"));
	}
	TEST_F(FMCPIntegrationTests, ReservesControlWorkersWhenCallsReachTheirLimit)
	{
		Register(); std::vector<std::future<httplib::Result>> Futures; std::vector<std::string> Sessions;
		std::array<std::atomic<bool>, 6> Opened{};
		for (size_t I = 0; I < Opened.size(); ++I)
		{
			if (I) Connect(); Sessions.push_back(TestSession);
			Futures.push_back(BeginCall([&, I] { Opened[I] = true; })); ASSERT_TRUE(Wait(Opened[I]));
		}
		Connect(); auto Limited = Post("tools/call", {{"name", "tests.echo"}}); ASSERT_TRUE(Limited); EXPECT_EQ(503, Limited->status);
		for (const auto& Session : Sessions)
		{
			TestSession = Session; auto Cancel = Notify("notifications/cancelled", {{"requestId", 1}}); ASSERT_TRUE(Cancel); EXPECT_EQ(202, Cancel->status);
		}
		for (auto& Future : Futures) (void)Future.get();
		Tools->TickTools(); EXPECT_EQ(0, Calls);
	}
	TEST_F(FMCPIntegrationTests, RequiresInitializedAndValidSessions)
	{
		httplib::Client Client("127.0.0.1", Port); auto Auth = Headers(); Auth.erase("Mcp-Session-Id");
		EXPECT_EQ(400, Client.Post("/mcp", Auth, Body("tools/list"), "application/json")->status);
		auto Init = Client.Post("/mcp", Auth, Body("initialize", InitializeParams()), "application/json"); ASSERT_TRUE(Init);
		Auth.emplace("Mcp-Session-Id", Init->get_header_value("Mcp-Session-Id"));
		EXPECT_EQ(400, Client.Post("/mcp", Auth, Body("tools/list"), "application/json")->status);
		EXPECT_EQ(202, Client.Post("/mcp", Auth, Serialize(FObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}), "application/json")->status);
		EXPECT_EQ(200, Client.Post("/mcp", Auth, Body("tools/list"), "application/json")->status);
		EXPECT_EQ(200, Client.Delete("/mcp", Auth)->status);
		EXPECT_EQ(404, Client.Post("/mcp", Auth, Body("tools/list"), "application/json")->status);
	}
	TEST_F(FMCPIntegrationTests, CancelsRunningCallCooperativelyWithoutBlockingTheTransport)
	{
		std::atomic<bool> Started = false, Cancelled = false;
		Register([&](const auto&, const auto& Context) {
			Started = true; const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!Context.Cancellation.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Cancelled = Context.Cancellation.IsCancellationRequested(); return FToolResult{EToolStatus::Succeeded, true};
		});
		auto Future = std::async(std::launch::async, [&] {
			httplib::Client Client("127.0.0.1", Port); httplib::Request Request;
			Request.method = "POST"; Request.path = "/mcp"; Request.headers = Headers();
			Request.set_header("Content-Type", "application/json"); Request.body = Body("tools/call", {{"name", "tests.echo"}});
			Request.content_receiver = [&](const char*, size_t, uint64_t, uint64_t) {
				EXPECT_TRUE(Wait(Started)); EXPECT_EQ(202, Notify("notifications/cancelled", {{"requestId", 1}})->status); return false;
			};
			return Client.send(Request);
		});
		(void)Pump(Future); EXPECT_EQ(1, Calls); EXPECT_TRUE(Cancelled);
	}
	TEST_F(FMCPIntegrationTests, MapsInvalidArgumentsAdmissionFailureDomainFailureAndInvalidOutput)
	{
		Register();
		auto Invalid = Post("tools/call", {{"name", "tests.echo"}, {"arguments", FObject{{"unknown", true}}}});
		ASSERT_TRUE(Invalid); EXPECT_NE(std::string::npos, Invalid->body.find("\"isError\":true")); EXPECT_EQ(0, Calls);
		auto Missing = Post("tools/call", {{"name", "tests.missing"}}); ASSERT_TRUE(Missing);
		EXPECT_NE(std::string::npos, Missing->body.find("-32602"));
		// Legacy advertised names are not aliases for the dot-separated catalog.
		auto Legacy = Post("tools/call", {{"name", "tests__echo"}}); ASSERT_TRUE(Legacy);
		EXPECT_NE(std::string::npos, Legacy->body.find("-32602")); EXPECT_EQ(0, Calls);
		FEditorToolLimits Limits; Limits.MaxQueued = 1; ASSERT_TRUE(Tools->GetService()->Configure(Limits));
		(void)Tools->ListToolsets();
		auto Occupied = Tools->GetService()->Submit({{"tests", "echo"}, 1}, EToolSource::External); ASSERT_TRUE(Occupied);
		auto Full = Post("tools/call", {{"name", "tests.echo"}}); ASSERT_TRUE(Full);
		EXPECT_NE(std::string::npos, Full->body.find("\"isError\":true"));
		ASSERT_TRUE(Tools->GetService()->Cancel(*Occupied)); Registration.Reset(); Toolset.reset();
		Register([](const auto&, const auto&) { return FToolResult{EToolStatus::Failed, {}, {{EToolError::DomainFailure, "domain rejected", "/field"}}}; });
		auto Failed = BeginCall(); auto Domain = Pump(Failed); ASSERT_TRUE(Domain);
		EXPECT_NE(std::string::npos, Domain->body.find("domain rejected")); EXPECT_NE(std::string::npos, Domain->body.find("/field"));
		Registration.Reset(); Toolset.reset();
		Register([](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, "wrong output"}; });
		auto BadOutput = BeginCall(); auto Output = Pump(BadOutput); ASSERT_TRUE(Output);
		EXPECT_NE(std::string::npos, Output->body.find("\"isError\":true"));
	}
	TEST_F(FMCPIntegrationTests, DoesNotRedirectAdmittedCallsToReplacementProviders)
	{
		Register(); std::atomic<bool> Opened = false;
		auto Future = BeginCall([&] { Opened = true; }); ASSERT_TRUE(Wait(Opened));
		Registration.Reset(); Toolset.reset(); Register();
		auto Response = Pump(Future); ASSERT_TRUE(Response); EXPECT_EQ(0, Calls);
		EXPECT_NE(std::string::npos, Response->body.find("\"isError\":true"));
	}
	TEST_F(FMCPIntegrationTests, CancelsDeadlinesAndServerStopBeforeExecution)
	{
		Register(); Server.Stop();
		auto Restart = Server.Start(*Host, {.BearerToken = std::string(Token), .CallTimeout = std::chrono::milliseconds(50)}); ASSERT_TRUE(Restart); Port = *Restart;
		Connect();
		auto Timed = Post("tools/call", {{"name", "tests.echo"}}); ASSERT_TRUE(Timed);
		EXPECT_NE(std::string::npos, Timed->body.find("deadline exceeded")); Tools->TickTools(); EXPECT_EQ(0, Calls);
		std::atomic<bool> Opened = false; auto Future = BeginCall([&] { Opened = true; }); ASSERT_TRUE(Wait(Opened));
		Server.Stop(); (void)Future.get(); Tools->TickTools(); EXPECT_EQ(0, Calls);
	}
	TEST_F(FMCPIntegrationTests, CompletionAndDisconnectRacesNeverRepeatDomainCalls)
	{
		Register();
		for (int I = 0; I < 12; ++I)
		{
			auto Future = std::async(std::launch::async, [&] {
				httplib::Client Client("127.0.0.1", Port); httplib::Request Request;
				Request.method = "POST"; Request.path = "/mcp"; Request.headers = Headers();
				Request.set_header("Content-Type", "application/json"); Request.body = Body("tools/call", {{"name", "tests.echo"}});
				Request.content_receiver = [](const char*, size_t, uint64_t, uint64_t) { return false; };
				return Client.send(Request);
			});
			(void)Pump(Future);
		}
		Server.Stop(); Tools->TickTools(); EXPECT_LE(Calls, 12);
	}
	TEST_F(FMCPIntegrationTests, ReportsClosedEditorAndExpiredCompletionWithoutRetrying)
	{
		Register(); FEditorToolLimits Limits; Limits.ResultTTL = std::chrono::milliseconds(0);
		ASSERT_TRUE(Tools->GetService()->Configure(Limits)); (void)Tools->ListToolsets();
		auto Future = BeginCall(); auto Expired = Pump(Future); ASSERT_TRUE(Expired);
		EXPECT_NE(std::string::npos, Expired->body.find("expired")); EXPECT_EQ(1, Calls);
		std::atomic<bool> Opened = false; auto Closing = BeginCall([&] { Opened = true; }); ASSERT_TRUE(Wait(Opened));
		Host->PrepareForShutdown(); auto Closed = Closing.get(); ASSERT_TRUE(Closed);
		EXPECT_NE(std::string::npos, Closed->body.find("\"isError\":true")); EXPECT_EQ(1, Calls);
	}
	// Only the explicit SDK acceptance driver enables this host. Ordinary native
	// runs and CTest discovery keep it disabled rather than reporting a skip.
	TEST_F(FMCPIntegrationTests, DISABLED_ServesOfficialSDKClient)
	{
		const char* Control = std::getenv("DURIN_MCP_SMOKE_CONTROL");
		const char* PortText = std::getenv("DURIN_MCP_SMOKE_PORT");
		ASSERT_NE(nullptr, Control) << "Use DevTool mcp sdk-test to launch the SDK host.";
		ASSERT_NE(nullptr, PortText) << "Use DevTool mcp sdk-test to launch the SDK host.";
		uint16 FixedPort = 0;
		auto [End, EC] = std::from_chars(PortText, PortText + std::strlen(PortText), FixedPort);
		ASSERT_EQ(std::errc{}, EC); ASSERT_EQ('\0', *End);
		Server.Stop();
		auto Started = Server.Start(*Host, {.Port = FixedPort, .BearerToken = std::string(Token)}); ASSERT_TRUE(Started);
		Port = FixedPort; Connect();
		auto* World = NewObject<DWorld>(Host, "MCPEditorWorld"); World->SetWorldType(EWorldType::Editor);
		ASSERT_TRUE(World->InitializeSubsystems());
		auto* Level = NewObject<DLevel>(World, "MCPEditorLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
		FEditorEngineTestAccess::SetEditorWorld(*Host, World);
		const auto Done = std::filesystem::path(Control) / "done";
		const auto Resume = std::filesystem::path(Control) / "resume";
		const auto Fixture = Testing::CreateTestFixtureDirectory("MCPSDK");
		const auto Marker = Fixture / "running";
		bool Paused = false, RunningCancelled = false; int QueuedExecutions = 0;
		auto Binding = [](std::string Name, EToolValueType Output) {
			return FToolBinding{FToolDescriptor{.Name = std::move(Name), .Output = FToolSchema(Output), .bExposeExternally = true}};
		};
		auto WaitTool = Binding("wait", EToolValueType::Boolean);
		WaitTool.Descriptor.Description = "Wait for cancellation. Marker: " + Marker.string();
		WaitTool.Handler = [&](const auto&, const auto& Context) {
			std::ofstream(Marker).put('1');
			const auto Until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!Context.Cancellation.IsCancellationRequested() && std::chrono::steady_clock::now() < Until)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			RunningCancelled = Context.Cancellation.IsCancellationRequested();
			return FToolResult{EToolStatus::Succeeded, RunningCancelled};
		};
		auto PauseTool = Binding("pause", EToolValueType::Boolean);
		PauseTool.Handler = [&](const auto&, const auto&) { Paused = true; return FToolResult{EToolStatus::Succeeded, true}; };
		auto QueuedTool = Binding("queued", EToolValueType::Boolean);
		QueuedTool.Handler = [&](const auto&, const auto&) { ++QueuedExecutions; return FToolResult{EToolStatus::Succeeded, true}; };
		auto StatsTool = Binding("stats", EToolValueType::Object);
		StatsTool.Descriptor.Output.Properties.emplace("runningCancelled", FToolSchema(EToolValueType::Boolean));
		StatsTool.Descriptor.Output.Properties.emplace("queuedExecutions", FToolSchema(EToolValueType::Integer));
		StatsTool.Handler = [&](const auto&, const auto&) {
			return FToolResult{EToolStatus::Succeeded, FObject{{"runningCancelled", RunningCancelled}, {"queuedExecutions", QueuedExecutions}}};
		};
		Toolset = std::make_unique<FNativeToolset>("tests", "SDK cancellation acceptance",
			std::vector<FToolBinding>{std::move(WaitTool), std::move(PauseTool), std::move(QueuedTool), std::move(StatsTool)});
		Registration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset); (void)Tools->ListToolsets();
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!std::filesystem::exists(Done) && std::chrono::steady_clock::now() < Deadline)
		{
			if (Paused && std::filesystem::exists(Resume))
			{
				// The SDK driver only resumes after the queued HTTP response was
				// opened and abandoned. Wait for native cancellation before ticking.
				if (Tools->GetService()->Configure(FEditorToolLimits{}))
				{
					(void)Tools->ListToolsets(); Paused = false; std::ofstream(Fixture / "resumed").put('1');
				}
			}
			if (!Paused) Tools->TickTools();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		EXPECT_TRUE(std::filesystem::exists(Done)) << "SDK harness did not finish within the deadline.";
		Server.Stop();
		FEditorEngineTestAccess::SetEditorWorld(*Host, nullptr);
		Testing::RemoveTestWorkDirectory(Fixture);
	}
	TEST_F(FMCPIntegrationTests, PendingNativeCompletionKeepsExistingWireResultUntilValidatedTick)
	{
		auto Source = FToolCompletionSource::CreateDetached(); auto Future = Tasks::Share(Source.TakeTask());
		FToolDescriptor Descriptor{.Name = "echo", .Output = FToolSchema(EToolValueType::Boolean), .bExposeExternally = true, .bMayPend = true};
		FToolBinding Binding{Descriptor};
		Binding.AsyncHandler = [this, Future](const auto&, const auto& Context) {
			++Calls; EXPECT_EQ(Host.Get(), GetToolEditor(Context)); return Future;
		};
		Toolset = std::make_unique<FNativeToolset>("tests", "Pending compatibility fixture", std::vector<FToolBinding>{std::move(Binding)});
		Registration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset); (void)Tools->ListToolsets();
		auto Pending = BeginCall();
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!Calls && std::chrono::steady_clock::now() < Deadline) { Tools->TickTools(); std::this_thread::yield(); }
		ASSERT_EQ(1, Calls); EXPECT_EQ(std::future_status::timeout, Pending.wait_for(std::chrono::milliseconds(0)));
		Source.TrySetValue({EToolStatus::Succeeded, true});
		EXPECT_EQ(std::future_status::timeout, Pending.wait_for(std::chrono::milliseconds(0)));
		auto Response = Pump(Pending); ASSERT_TRUE(Response); EXPECT_EQ(200, Response->status);
		EXPECT_NE(std::string::npos, Response->body.find("\"isError\":false"));
		EXPECT_NE(std::string::npos, Response->body.find("\"data\":true"));
		EXPECT_EQ(1, Calls); EXPECT_EQ(0u, Host->GetTransactor()->GetQueueLength());
	}

}
