#pragma once

#include "CoreMinimal.h"
#include "HTTPServerAPI.h"
#include <exception>

namespace Durin
{
	// Borrowed request view, valid only for the duration of its handler/filter.
	class FHTTPRequest final
	{
	public:
		HTTPSERVER_API auto GetHeaderValue(const std::string& Name) const -> std::string;
		HTTPSERVER_API auto GetHeaderValueCount(const std::string& Name) const -> size_t;
		HTTPSERVER_API auto HasHeader(const std::string& Name) const -> bool;
		HTTPSERVER_API auto GetBody() const -> std::string_view;
		HTTPSERVER_API auto GetMethod() const -> std::string_view;
		HTTPSERVER_API auto GetPath() const -> std::string_view;
		HTTPSERVER_API auto GetLocalPort() const -> uint16;
	private:
		friend class FHTTPServer;
		struct FImpl;
		explicit FHTTPRequest(const FImpl& InImpl) : Impl(InImpl) {}
		const FImpl& Impl;
	};

	// Borrowed streaming writer; never retain it beyond a provider invocation.
	class FHTTPStreamSink final
	{
	public:
		HTTPSERVER_API auto IsWritable() const -> bool;
		HTTPSERVER_API auto Write(std::string_view Bytes) -> bool;
		HTTPSERVER_API auto Done() -> void;
	private:
		friend class FHTTPResponse;
		struct FImpl;
		explicit FHTTPStreamSink(FImpl& InImpl) : Impl(InImpl) {}
		FImpl& Impl;
	};

	// Borrowed response builder. Providers run later on the same request worker.
	class FHTTPResponse final
	{
	public:
		using FChunkProvider = std::function<bool(size_t Offset, FHTTPStreamSink& Sink)>;
		using FStreamComplete = std::function<void(bool Success)>;
		HTTPSERVER_API auto SetStatus(int Status) -> void;
		HTTPSERVER_API auto SetHeader(const std::string& Name, const std::string& Value) -> void;
		// Body setters replace prior body/framing. A replaced stream is completed
		// with false on this worker before its provider captures are released.
		HTTPSERVER_API auto SetContent(std::string Body, const std::string& ContentType) -> void;
		// Offset counts bytes written. Return false to abort; call Done to finish.
		// Complete runs once after provider use ends, on replacement or response
		// release (including disconnect/stop). Captures must survive until then;
		// completion callbacks must not throw. Provider exceptions abort the connection.
		HTTPSERVER_API auto SetChunkedContentProvider(const std::string& ContentType,
			FChunkProvider Provider, FStreamComplete Complete = {}) -> void;
	private:
		friend class FHTTPServer;
		struct FImpl;
		explicit FHTTPResponse(FImpl& InImpl) : Impl(InImpl) {}
		FImpl& Impl;
	};

	enum class EHTTPMethod { Get, Post, Delete };
	using FHTTPHandler = std::function<void(const FHTTPRequest&, FHTTPResponse&)>;
	// Routes match an exact path; filters run first, including for unmatched paths.
	struct FHTTPRoute
	{
		EHTTPMethod Method;
		std::string Path;
		FHTTPHandler Handler;
	};
	// One server's transport limits; no authentication or application protocol policy.
	struct FHTTPServerOptions
	{
		std::string BindAddress = "127.0.0.1";
		uint16 Port = 0;
		size_t WorkerCount = 8;
		size_t MaxQueuedConnections = 8;
		size_t MaxBodyBytes = 1024 * 1024;
		std::chrono::milliseconds ReadTimeout{2000};
		std::chrono::milliseconds WriteTimeout{2000};
		size_t KeepAliveMaxCount = 1;
	};
	// Owns the listener, workers and sockets. Start/Stop/destruction are serialized
	// by the caller, outside callbacks. Callbacks may run concurrently on workers.
	class FHTTPServer final
	{
	public:
		using FRequestFilter = std::function<bool(const FHTTPRequest&, FHTTPResponse&)>;
		using FExceptionHandler = std::function<void(FHTTPResponse&, std::exception_ptr)>;
		HTTPSERVER_API FHTTPServer();
		HTTPSERVER_API ~FHTTPServer();
		FHTTPServer(const FHTTPServer&) = delete;
		auto operator=(const FHTTPServer&) -> FHTTPServer& = delete;
		// A true filter result completes the request without running a route.
		// Returns the bound port after workers and listener are ready; port zero
		// selects a free port. Thread startup failures return an error and release the port.
		HTTPSERVER_API auto Start(const FHTTPServerOptions& Options, std::vector<FHTTPRoute> Routes,
			FRequestFilter Filter = {}, FExceptionHandler OnException = {}) -> std::expected<uint16, std::string>;
		// Interrupts partial reads and streams, joins workers, and releases callbacks
		// before returning. Idempotent. Callbacks must cooperate with shutdown.
		HTTPSERVER_API auto Stop() noexcept -> void;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
