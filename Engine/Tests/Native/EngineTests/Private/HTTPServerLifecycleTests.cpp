#include <httplib.h>
#include "HTTPServer.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	auto MakeClient(uint16 Port, std::chrono::seconds ReadTimeout = std::chrono::seconds(2)) -> httplib::Client
	{
		httplib::Client Client("127.0.0.1", Port);
		Client.set_read_timeout(ReadTimeout);
		return Client;
	}

	auto BeginGet(uint16 Port, std::string Path, std::chrono::seconds ReadTimeout = std::chrono::seconds(2)) -> std::future<httplib::Result>
	{
		return std::async(std::launch::async, [Port, Path = std::move(Path), ReadTimeout] {
			auto Client = MakeClient(Port, ReadTimeout);
			return Client.Get(Path);
		});
	}

	// Declare after both async futures so early returns release the worker before
	// either future's destructor can wait for it. Release is control-thread only.
	class FScopedWorkerRelease final
	{
	public:
		explicit FScopedWorkerRelease(std::promise<void>& InSignal) : Signal(InSignal) {}
		~FScopedWorkerRelease() { Release(); }
		FScopedWorkerRelease(const FScopedWorkerRelease&) = delete;
		auto operator=(const FScopedWorkerRelease&) -> FScopedWorkerRelease& = delete;
		auto Release() -> void { if (!std::exchange(Released, true)) Signal.set_value(); }
	private:
		std::promise<void>& Signal;
		bool Released = false;
	};

	// Owns a raw loopback peer even when an assertion returns from the test early.
	class FScopedSocket final
	{
	public:
		FScopedSocket() : Socket(::socket(AF_INET, SOCK_STREAM, 0)) {}
		~FScopedSocket() { if (Socket != INVALID_SOCKET) httplib::detail::close_socket(Socket); }
		FScopedSocket(const FScopedSocket&) = delete;
		auto operator=(const FScopedSocket&) -> FScopedSocket& = delete;
		auto Get() const -> socket_t { return Socket; }
	private:
		socket_t Socket;
	};

	auto WaitFor(const std::atomic<int>& Value, int Expected) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (Value != Expected && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return Value == Expected;
	}

	TEST(FHTTPServerLifecycleTests, RoutesExactPathsAndPreservesHeaderAndBodyValues)
	{
		FHTTPServer Server;
		auto Started = Server.Start({}, {
			{EHTTPMethod::Post, "/literal.:name", [](const auto& Request, auto& Response) {
				EXPECT_EQ("POST", Request.GetMethod()); EXPECT_EQ("/literal.:name", Request.GetPath());
				EXPECT_EQ(2u, Request.GetHeaderValueCount("x-value")); EXPECT_TRUE(Request.HasHeader("X-VALUE"));
				EXPECT_EQ("first", Request.GetHeaderValue("X-Value")); EXPECT_FALSE(Request.HasHeader("Missing"));
				Response.SetStatus(201); Response.SetHeader("X-Answer", "ok");
				Response.SetContent(std::string(Request.GetBody()), "application/octet-stream");
			}},
			{EHTTPMethod::Delete, "/literal.:name", [](const auto&, auto& Response) { Response.SetStatus(204); }}
		}, [](const auto& Request, auto&) { EXPECT_GT(Request.GetLocalPort(), 0); return false; });
		ASSERT_TRUE(Started) << Started.error();
		auto Client = MakeClient(*Started);
		const std::string Body("hello\0world", 11);
		auto Result = Client.Post("/literal.:name", {{"X-Value", "first"}, {"X-Value", "second"}}, Body, "application/octet-stream");
		ASSERT_TRUE(Result); EXPECT_EQ(201, Result->status); EXPECT_EQ(Body, Result->body);
		EXPECT_EQ("ok", Result->get_header_value("X-Answer"));
		auto DifferentPath = Client.Post("/literalX:name", "", "text/plain"); ASSERT_TRUE(DifferentPath); EXPECT_EQ(404, DifferentPath->status);
		auto ParameterPath = Client.Post("/literal.anything", "", "text/plain"); ASSERT_TRUE(ParameterPath); EXPECT_EQ(404, ParameterPath->status);
		auto Deleted = Client.Delete("/literal.:name"); ASSERT_TRUE(Deleted); EXPECT_EQ(204, Deleted->status);
	}

	TEST(FHTTPServerLifecycleTests, AppliesFilterBodyLimitAndApplicationExceptionHandler)
	{
		std::atomic<int> Calls{0};
		FHTTPServer Server; FHTTPServerOptions Options; Options.MaxBodyBytes = 4;
		auto Started = Server.Start(Options, {{EHTTPMethod::Post, "/error", [&](const auto&, auto&) {
			++Calls; throw std::runtime_error("application failure");
		}}}, [](const auto& Request, auto& Response) {
			if (Request.GetHeaderValue("X-Deny") != "yes") return false;
			Response.SetStatus(403); return true;
		}, [](auto& Response, std::exception_ptr Error) {
			EXPECT_TRUE(Error); Response.SetStatus(500); Response.SetContent("handled", "text/plain");
		});
		ASSERT_TRUE(Started) << Started.error();
		auto Client = MakeClient(*Started);
		auto Denied = Client.Get("/unknown", httplib::Headers{{"X-Deny", "yes"}}); ASSERT_TRUE(Denied); EXPECT_EQ(403, Denied->status);
		auto Large = Client.Post("/error", "12345", "text/plain"); ASSERT_TRUE(Large); EXPECT_EQ(413, Large->status); EXPECT_EQ(0, Calls);
		auto Error = Client.Post("/error", "1234", "text/plain"); ASSERT_TRUE(Error);
		EXPECT_EQ(500, Error->status); EXPECT_EQ("handled", Error->body); EXPECT_EQ(1, Calls);
	}

	TEST(FHTTPServerLifecycleTests, StreamsBytesAndCompletesExactlyOnce)
	{
		std::atomic<int> Completed{0}; std::atomic<bool> Success{false}; FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/stream", [&](const auto&, auto& Response) {
			Response.SetChunkedContentProvider("text/event-stream", [](size_t Offset, FHTTPStreamSink& Sink) {
				EXPECT_TRUE(Sink.IsWritable());
				if (Offset == 0) return Sink.Write(": connected\n\n");
				EXPECT_EQ(13u, Offset);
				if (!Sink.Write("data: done\n\n")) return false;
				Sink.Done(); return true;
			}, [&](bool OK) { Success = OK; ++Completed; });
		}}});
		ASSERT_TRUE(Started) << Started.error();
		auto Client = MakeClient(*Started);
		auto Result = Client.Get("/stream"); ASSERT_TRUE(Result);
		EXPECT_EQ("text/event-stream", Result->get_header_value("Content-Type"));
		EXPECT_EQ(": connected\n\ndata: done\n\n", Result->body);
		EXPECT_TRUE(WaitFor(Completed, 1)); EXPECT_TRUE(Success);
		Server.Stop(); EXPECT_EQ(1, Completed);
	}

	TEST(FHTTPServerLifecycleTests, StopInterruptsStreamAndReleasesCallbacksBeforeRestart)
	{
		std::atomic<int> Opened{0}, Completed{0}; std::atomic<bool> Success{true};
		auto Lifetime = std::make_shared<int>(1); std::weak_ptr<int> Weak = Lifetime;
		FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/stream", [&, Lifetime](const auto&, auto& Response) {
			Response.SetChunkedContentProvider("text/event-stream", [&](size_t Offset, FHTTPStreamSink& Sink) {
				if (!Sink.IsWritable()) return false;
				if (Offset == 0) { Opened = 1; return Sink.Write(": open\n\n"); }
				std::this_thread::sleep_for(std::chrono::milliseconds(5)); return true;
			}, [&](bool OK) { Success = OK; ++Completed; });
		}}});
		ASSERT_TRUE(Started) << Started.error(); Lifetime.reset();
		auto Future = BeginGet(*Started, "/stream");
		EXPECT_TRUE(WaitFor(Opened, 1));
		const auto Begin = std::chrono::steady_clock::now(); Server.Stop();
		EXPECT_LT(std::chrono::steady_clock::now() - Begin, std::chrono::seconds(2));
		(void)Future.get(); EXPECT_EQ(1, Completed); EXPECT_FALSE(Success); EXPECT_TRUE(Weak.expired());
		Server.Stop();
		FHTTPServerOptions Options; Options.Port = *Started;
		auto Restarted = Server.Start(Options, {}); ASSERT_TRUE(Restarted); EXPECT_EQ(*Started, *Restarted);
	}

	TEST(FHTTPServerLifecycleTests, FailedStartReleasesPortAndAllowsRetry)
	{
		FHTTPServer First, Second;
		FHTTPServerOptions Invalid; Invalid.WorkerCount = 0; EXPECT_FALSE(First.Start(Invalid, {}));
		auto Started = First.Start({}, {}); ASSERT_TRUE(Started);
		EXPECT_FALSE(First.Start({}, {}));
		FHTTPServerOptions Options; Options.Port = *Started; EXPECT_FALSE(Second.Start(Options, {}));
		First.Stop();
		EXPECT_TRUE(Second.Start(Options, {}));
	}

	TEST(FHTTPServerLifecycleTests, ReplacingStreamCompletesOldProviderAndReplacesContentType)
	{
		std::atomic<int> OldCompleted{0}, OldCalls{0}, NewCompleted{0};
		FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/replace", [&](const auto&, auto& Response) {
			auto Lifetime = std::make_shared<int>(1); std::weak_ptr<int> Weak = Lifetime;
			Response.SetChunkedContentProvider("text/event-stream", [&, Lifetime](size_t, auto&) { ++OldCalls; return false; },
				[&, Weak](bool OK) { EXPECT_FALSE(OK); EXPECT_FALSE(Weak.expired()); ++OldCompleted; });
			Lifetime.reset();
			Response.SetChunkedContentProvider("text/plain", [](size_t, auto& Sink) {
				if (!Sink.Write("replacement")) return false;
				Sink.Done(); return true;
			}, [&](bool OK) { EXPECT_TRUE(OK); ++NewCompleted; });
			EXPECT_TRUE(Weak.expired());
		}}});
		ASSERT_TRUE(Started);
		auto Client = MakeClient(*Started);
		auto Result = Client.Get("/replace"); ASSERT_TRUE(Result);
		Server.Stop();
		EXPECT_EQ("replacement", Result->body); EXPECT_EQ("text/plain", Result->get_header_value("Content-Type"));
		EXPECT_EQ(1u, Result->get_header_value_count("Content-Type"));
		EXPECT_EQ(0, OldCalls); EXPECT_EQ(1, OldCompleted); EXPECT_EQ(1, NewCompleted);
	}

	TEST(FHTTPServerLifecycleTests, FixedBodyAndErrorResponseRetirePreviouslyInstalledStreams)
	{
		std::atomic<int> Completed{0}, ProviderCalls{0};
		FHTTPServer Server;
		auto Handler = [&](const FHTTPRequest& Request, FHTTPResponse& Response) {
			Response.SetChunkedContentProvider("text/event-stream", [&](size_t, auto& Sink) {
				++ProviderCalls; if (!Sink.Write("stale")) return false; Sink.Done(); return true;
			}, [&](bool OK) { EXPECT_FALSE(OK); ++Completed; });
			if (Request.GetPath() == "/error") throw std::runtime_error("after stream installation");
			Response.SetContent(Request.GetPath() == "/empty" ? "" : "fixed", "text/plain");
		};
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/fixed", Handler}, {EHTTPMethod::Get, "/empty", Handler},
			{EHTTPMethod::Get, "/error", Handler}}, {}, [](auto& Response, std::exception_ptr) {
				Response.SetStatus(500); Response.SetContent("handled", "text/plain");
			});
		ASSERT_TRUE(Started);
		auto Client = MakeClient(*Started);
		for (const auto& [Path, Expected] : std::array<std::pair<const char*, const char*>, 3>{{{"/fixed", "fixed"}, {"/empty", ""}, {"/error", "handled"}}})
		{
			auto Result = Client.Get(Path); ASSERT_TRUE(Result) << Path;
			EXPECT_EQ(Expected, Result->body) << Path;
			EXPECT_EQ(std::string_view(Path) == "/error" ? 500 : 200, Result->status);
			EXPECT_FALSE(Result->has_header("Transfer-Encoding"));
		}
		Server.Stop(); EXPECT_EQ(0, ProviderCalls); EXPECT_EQ(3, Completed);
	}

	TEST(FHTTPServerLifecycleTests, WorkerAllocationFailureReturnsErrorAndReleasesBoundPort)
	{
		// reserve(SIZE_MAX) fails before any threads are created. Keep the probe in
		// a child so an uncaught listener-thread exception is reported as a test failure.
		EXPECT_EXIT({
			FHTTPServer Server; auto Initial = Server.Start({}, {});
			if (!Initial) std::exit(2);
			Server.Stop(); FHTTPServerOptions Options; Options.Port = *Initial;
			Options.WorkerCount = std::numeric_limits<size_t>::max();
			if (Server.Start(Options, {})) std::exit(3);
			Options.WorkerCount = 1;
			if (!Server.Start(Options, {})) std::exit(4);
			Server.Stop(); std::exit(0);
		}, testing::ExitedWithCode(0), "");
	}

	TEST(FHTTPServerLifecycleTests, StreamReplacesFixedBodyAndStaleFraming)
	{
		FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/stream", [](const auto&, auto& Response) {
			Response.SetContent("stale", "application/json"); Response.SetHeader("Content-Length", "5");
			Response.SetChunkedContentProvider("text/plain", [](size_t, auto& Sink) {
				if (!Sink.Write("replacement")) return false; Sink.Done(); return true;
			});
		}}});
		ASSERT_TRUE(Started);
		auto Client = MakeClient(*Started);
		auto Result = Client.Get("/stream"); ASSERT_TRUE(Result);
		EXPECT_EQ("replacement", Result->body); EXPECT_EQ("text/plain", Result->get_header_value("Content-Type"));
		EXPECT_EQ(1u, Result->get_header_value_count("Content-Type")); EXPECT_FALSE(Result->has_header("Content-Length"));
	}

	TEST(FHTTPServerLifecycleTests, UnhandledRoutingExceptionRetiresStreamBeforeDefaultError)
	{
		std::atomic<int> Completed{0}, ProviderCalls{0}; FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/error", [&](const auto&, auto& Response) {
			Response.SetHeader("X-Policy", "preserved");
			Response.SetChunkedContentProvider("text/plain", [&](size_t, auto& Sink) {
				++ProviderCalls; Sink.Done(); return true;
			}, [&](bool OK) { EXPECT_FALSE(OK); ++Completed; });
			throw std::runtime_error("after stream installation");
		}}});
		ASSERT_TRUE(Started);
		auto Client = MakeClient(*Started);
		auto Result = Client.Get("/error"); ASSERT_TRUE(Result); EXPECT_EQ(500, Result->status);
		EXPECT_TRUE(Result->body.empty()); EXPECT_FALSE(Result->has_header("Transfer-Encoding"));
		EXPECT_EQ("preserved", Result->get_header_value("X-Policy"));
		Server.Stop(); EXPECT_EQ(0, ProviderCalls); EXPECT_EQ(1, Completed);
	}

	TEST(FHTTPServerLifecycleTests, StopWaitsForHandlerAndCompletionBeforeReleasingCaptures)
	{
		std::atomic<int> Active{0}, Completed{0};
		std::promise<void> Release; auto Gate = Release.get_future().share();
		auto Lifetime = std::make_shared<int>(1); std::weak_ptr<int> Weak = Lifetime;
		FHTTPServer Server;
		auto Started = Server.Start({}, {{EHTTPMethod::Get, "/blocked", [&, Lifetime](const auto&, auto& Response) {
			const auto Worker = std::this_thread::get_id();
			Response.SetChunkedContentProvider("text/plain", [](size_t, auto& Sink) { Sink.Done(); return true; },
				[&, Worker](bool OK) { EXPECT_EQ(Worker, std::this_thread::get_id()); EXPECT_FALSE(OK); ++Completed; });
			Active = 1; Gate.wait(); Active = 0;
		}}});
		ASSERT_TRUE(Started); Lifetime.reset();
		auto Client = BeginGet(*Started, "/blocked", std::chrono::seconds(3));
		std::future<void> Stop;
		FScopedWorkerRelease WorkerRelease(Release);
		ASSERT_TRUE(WaitFor(Active, 1));
		Stop = std::async(std::launch::async, [&] { Server.Stop(); });
		const bool Waited = Stop.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
		WorkerRelease.Release(); Stop.get(); (void)Client.get();
		EXPECT_TRUE(Waited); EXPECT_EQ(0, Active); EXPECT_EQ(1, Completed); EXPECT_TRUE(Weak.expired());
	}

	TEST(FHTTPServerLifecycleTests, ProviderExceptionCompletesOnceAndLeavesServerUsable)
	{
		std::atomic<int> Completed{0}; FHTTPServer Server;
		auto Started = Server.Start({}, {
			{EHTTPMethod::Get, "/throw", [&](const auto&, auto& Response) {
				Response.SetChunkedContentProvider("text/plain", [](size_t Offset, auto& Sink) -> bool {
					if (Offset == 0) return Sink.Write("first");
					throw std::runtime_error("stream provider failure");
				}, [&](bool OK) { EXPECT_FALSE(OK); ++Completed; });
			}},
			{EHTTPMethod::Get, "/ok", [](const auto&, auto& Response) { Response.SetContent("ok", "text/plain"); }}
		});
		ASSERT_TRUE(Started);
		auto Client = MakeClient(*Started);
		EXPECT_FALSE(Client.Get("/throw")); EXPECT_TRUE(WaitFor(Completed, 1));
		auto Result = Client.Get("/ok"); ASSERT_TRUE(Result); EXPECT_EQ("ok", Result->body);
		Server.Stop(); EXPECT_EQ(1, Completed);
	}

	TEST(FHTTPServerLifecycleTests, StopDrainsQueuedFullAndPartialRequestsWithoutInvokingHandlers)
	{
		std::atomic<int> Active{0}, QueuedCalls{0};
		std::promise<void> Release; auto Gate = Release.get_future().share();
		FHTTPServer Server; FHTTPServerOptions Options;
		Options.WorkerCount = 1; Options.MaxQueuedConnections = 2; Options.ReadTimeout = std::chrono::seconds(10);
		auto Started = Server.Start(Options, {
			{EHTTPMethod::Get, "/blocked", [&](const auto&, auto&) { Active = 1; Gate.wait(); }},
			{EHTTPMethod::Post, "/queued", [&](const auto&, auto&) { ++QueuedCalls; }}
		});
		ASSERT_TRUE(Started);
		auto Client = BeginGet(*Started, "/blocked", std::chrono::seconds(3));
		std::future<void> Stop;
		FScopedWorkerRelease WorkerRelease(Release);
		ASSERT_TRUE(WaitFor(Active, 1));
		std::array<FScopedSocket, 2> Sockets;
		for (size_t I = 0; I < Sockets.size(); ++I)
		{
			const auto Socket = Sockets[I].Get(); ASSERT_NE(INVALID_SOCKET, Socket);
			sockaddr_in Address{}; Address.sin_family = AF_INET; Address.sin_port = htons(*Started); Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			ASSERT_EQ(0, ::connect(Socket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)));
			const auto Wire = "POST /queued HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(*Started)
				+ "\r\nContent-Length: " + (I == 0 ? "100" : "1") + "\r\n\r\nx";
			ASSERT_GT(::send(Socket, Wire.data(), static_cast<int>(Wire.size()), 0), 0);
		}
		Stop = std::async(std::launch::async, [&] { Server.Stop(); });
		// The blocked peer observing EOF proves Stop has interrupted active sockets
		// before we let its worker pick up any queued requests.
		EXPECT_EQ(std::future_status::ready, Client.wait_for(std::chrono::seconds(1)));
		const auto Begin = std::chrono::steady_clock::now(); WorkerRelease.Release(); Stop.get(); (void)Client.get();
		EXPECT_LT(std::chrono::steady_clock::now() - Begin, std::chrono::seconds(2)); EXPECT_EQ(0, QueuedCalls);
	}
}
