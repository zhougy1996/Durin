#include <httplib.h>
#include "HTTPServer.h"

namespace Durin
{
	namespace
	{
		// Retire an unsent body before replacing it, including during exception
		// translation. Keep provider captures alive through their completion callback.
		auto ResetResponseContent(httplib::Response& Response) -> void
		{
			auto Provider = std::exchange(Response.content_provider_, {});
			auto Complete = std::exchange(Response.content_provider_resource_releaser_, {});
			Response.body.clear(); Response.content_length_ = 0;
			Response.is_chunked_content_provider_ = false;
			Response.content_provider_success_ = false;
			Response.headers.erase("Content-Type");
			Response.headers.erase("Content-Length");
			Response.headers.erase("Transfer-Encoding");
			if (Complete) Complete(false);
		}
	}

	struct FHTTPRequest::FImpl { const httplib::Request& Request; };
	struct FHTTPResponse::FImpl { httplib::Response& Response; };
	struct FHTTPStreamSink::FImpl { httplib::DataSink& Sink; };

	auto FHTTPRequest::GetHeaderValue(const std::string& Name) const -> std::string { return Impl.Request.get_header_value(Name); }
	auto FHTTPRequest::GetHeaderValueCount(const std::string& Name) const -> size_t { return Impl.Request.get_header_value_count(Name); }
	auto FHTTPRequest::HasHeader(const std::string& Name) const -> bool { return Impl.Request.has_header(Name); }
	auto FHTTPRequest::GetBody() const -> std::string_view { return Impl.Request.body; }
	auto FHTTPRequest::GetMethod() const -> std::string_view { return Impl.Request.method; }
	auto FHTTPRequest::GetPath() const -> std::string_view { return Impl.Request.path; }
	auto FHTTPRequest::GetLocalPort() const -> uint16 { return static_cast<uint16>(Impl.Request.local_port); }
	auto FHTTPStreamSink::IsWritable() const -> bool { return Impl.Sink.is_writable(); }
	auto FHTTPStreamSink::Write(std::string_view Bytes) -> bool { return Impl.Sink.write(Bytes.data(), Bytes.size()); }
	auto FHTTPStreamSink::Done() -> void { Impl.Sink.done(); }
	auto FHTTPResponse::SetStatus(int Status) -> void { Impl.Response.status = Status; }
	auto FHTTPResponse::SetHeader(const std::string& Name, const std::string& Value) -> void { Impl.Response.set_header(Name, Value); }
	auto FHTTPResponse::SetContent(std::string Body, const std::string& ContentType) -> void
	{
		ResetResponseContent(Impl.Response);
		Impl.Response.set_content(std::move(Body), ContentType);
	}
	auto FHTTPResponse::SetChunkedContentProvider(const std::string& ContentType,
		FChunkProvider Provider, FStreamComplete Complete) -> void
	{
		ResetResponseContent(Impl.Response);
		Impl.Response.set_chunked_content_provider(ContentType,
			[Provider = std::move(Provider)](size_t Offset, httplib::DataSink& Native) {
				FHTTPStreamSink::FImpl State{Native}; FHTTPStreamSink Sink(State);
				return Provider(Offset, Sink);
			}, std::move(Complete));
	}

	namespace
	{
		// Owns descriptor closure so Stop can interrupt partial reads without
		// racing descriptor reuse. Only this backend depends on the pinned serving seam.
		class FInterruptibleHTTPServer final : public httplib::Server
		{
		public:
			auto StopConnections() -> void
			{
				std::lock_guard Lock(Mutex); Closing = true;
				for (auto Socket : Sockets) httplib::detail::shutdown_socket(Socket);
			}
		protected:
			auto process_and_close_socket(socket_t Socket) -> bool override
			{
				{
					std::lock_guard Lock(Mutex); Sockets.insert(Socket);
					if (Closing) httplib::detail::shutdown_socket(Socket);
				}
				struct FScope
				{
					FInterruptibleHTTPServer& Owner; socket_t Socket;
					~FScope()
					{
						std::lock_guard Lock(Owner.Mutex); Owner.Sockets.erase(Socket);
						httplib::detail::drain_and_close_socket(Socket);
					}
				} Scope{*this, Socket};
				std::string Remote, Local; int RemotePort = 0, LocalPort = 0;
				httplib::detail::get_remote_ip_and_port(Socket, Remote, RemotePort);
				httplib::detail::get_local_ip_and_port(Socket, Local, LocalPort);
				return serve_guarded([&] {
					return httplib::detail::process_server_socket(svr_sock_, Socket, keep_alive_max_count_, keep_alive_timeout_sec_,
						read_timeout_sec_, read_timeout_usec_, write_timeout_sec_, write_timeout_usec_,
						[&](httplib::Stream& Stream, bool Close, bool& Closed) {
							return process_request(Stream, Remote, RemotePort, Local, LocalPort, Close, Closed, nullptr);
						});
				});
			}
		private:
			std::mutex Mutex;
			std::set<socket_t> Sockets;
			bool Closing = false;
		};

		auto ExactPathPattern(std::string_view Path) -> std::string
		{
			std::string Pattern;
			for (char C : Path)
			{
				// Avoid the backend's /:parameter route syntax as well as regex syntax.
				if (C == ':') { Pattern += "\\x3a"; continue; }
				if (std::string_view(R"(\.^$|()[]{}*+?)").find(C) != std::string_view::npos) Pattern += '\\';
				Pattern += C;
			}
			return Pattern;
		}
	}

	struct FHTTPServer::FImpl
	{
		FInterruptibleHTTPServer HTTP;
		// Owned here until the listener takes it. Failed thread startup must join
		// these workers too, even though the backend never consumed the queue.
		std::unique_ptr<httplib::ThreadPool> Workers;
		std::thread Listener;
		~FImpl()
		{
			HTTP.stop(); HTTP.StopConnections();
			if (Listener.joinable()) Listener.join();
			if (Workers) Workers->shutdown();
		}
		template<typename F>
		static auto WithViews(const httplib::Request& Wire, httplib::Response& Native, const F& Callback)
		{
			FHTTPRequest::FImpl RequestState{Wire}; FHTTPRequest Request(RequestState);
			FHTTPResponse::FImpl ResponseState{Native}; FHTTPResponse Response(ResponseState);
			try { return Callback(Request, Response); }
			catch (...)
			{
				ResetResponseContent(Native);
				throw;
			}
		}
	};

	FHTTPServer::FHTTPServer() = default;
	FHTTPServer::~FHTTPServer() { Stop(); }
	auto FHTTPServer::Start(const FHTTPServerOptions& Options, std::vector<FHTTPRoute> Routes,
		FRequestFilter Filter, FExceptionHandler OnException) -> std::expected<uint16, std::string>
	{
		if (Impl) return std::unexpected("HTTP server is already started.");
		if (Options.BindAddress.empty() || Options.WorkerCount == 0 || Options.MaxQueuedConnections == 0
			|| Options.MaxBodyBytes == 0 || Options.KeepAliveMaxCount == 0
			|| Options.ReadTimeout <= std::chrono::milliseconds(0) || Options.WriteTimeout <= std::chrono::milliseconds(0))
			return std::unexpected("HTTP server requires a bind address and positive resource limits/timeouts.");
		for (const auto& Route : Routes)
		{
			if (Route.Path.empty() || Route.Path.front() != '/' || !Route.Handler
				|| (Route.Method != EHTTPMethod::Get && Route.Method != EHTTPMethod::Post && Route.Method != EHTTPMethod::Delete))
				return std::unexpected("HTTP route requires a supported method, absolute path and handler.");
		}
		auto State = std::make_unique<FImpl>();
		auto& HTTP = State->HTTP;
		// Each server exclusively owns its endpoint; override upstream reuse-port defaults.
		HTTP.set_socket_options([](socket_t Socket) {
#if defined(_WIN32)
			const int Exclusive = 1;
			setsockopt(Socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&Exclusive), sizeof(Exclusive));
#else
			const int ReuseAddress = 1;
			setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, &ReuseAddress, sizeof(ReuseAddress));
#endif
		});
		HTTP.new_task_queue = [Raw = State.get()] { return Raw->Workers.release(); };
		HTTP.set_payload_max_length(Options.MaxBodyBytes);
		HTTP.set_read_timeout(Options.ReadTimeout);
		HTTP.set_write_timeout(Options.WriteTimeout);
		HTTP.set_keep_alive_max_count(Options.KeepAliveMaxCount);
		if (Filter) HTTP.set_pre_routing_handler([Filter = std::move(Filter)](const auto& Request, auto& Response) {
			return FImpl::WithViews(Request, Response, Filter)
				? httplib::Server::HandlerResponse::Handled : httplib::Server::HandlerResponse::Unhandled;
		});
		for (auto& Route : Routes)
		{
			auto Handler = [Callback = std::move(Route.Handler)](const httplib::Request& Request, httplib::Response& Response) {
				FImpl::WithViews(Request, Response, Callback);
			};
			const auto Path = ExactPathPattern(Route.Path);
			switch (Route.Method)
			{
			case EHTTPMethod::Get: HTTP.Get(Path, std::move(Handler)); break;
			case EHTTPMethod::Post: HTTP.Post(Path, std::move(Handler)); break;
			case EHTTPMethod::Delete: HTTP.Delete(Path, std::move(Handler)); break;
			}
		}
		if (OnException) HTTP.set_exception_handler([OnException = std::move(OnException)](const auto&, auto& Native, std::exception_ptr Error) {
			FHTTPResponse::FImpl ResponseState{Native}; FHTTPResponse Response(ResponseState);
			OnException(Response, Error);
		});
		int Port = Options.Port;
		if (Port == 0) Port = HTTP.bind_to_any_port(Options.BindAddress);
		else if (!HTTP.bind_to_port(Options.BindAddress, Port)) Port = -1;
		if (Port <= 0) return std::unexpected("Could not bind the HTTP port.");
		try
		{
			// Construct on the control thread: backend readiness is signalled before
			// new_task_queue runs, and exceptions on the listener would terminate.
			State->Workers = std::make_unique<httplib::ThreadPool>(Options.WorkerCount,
				Options.WorkerCount, Options.MaxQueuedConnections);
			State->Listener = std::thread([Raw = State.get()] { Raw->HTTP.listen_after_bind(); });
		}
		catch (const std::exception& Error)
		{
			return std::unexpected(std::string("Could not start HTTP threads: ") + Error.what());
		}
		HTTP.wait_until_ready();
		if (!HTTP.is_running()) return std::unexpected("HTTP listener failed to start.");
		Impl = std::move(State);
		return static_cast<uint16>(Port);
	}
	auto FHTTPServer::Stop() noexcept -> void { Impl.reset(); }
}
