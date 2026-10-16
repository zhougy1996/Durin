# HTTP Server

Summary: Instance-owned HTTP transport with exact-path routes, bounded resources, chunked responses, and interruptible shutdown.
Modules: HTTPServer, Core

Last reviewed: 2026-10-16

## Ownership

`HTTPServer` is an opt-in Runtime module depending publicly on Core and privately
on the pinned cpp-httplib backend. Its public API is
[`HTTPServer.h`](../../../Engine/Source/Runtime/HTTPServer/Public/HTTPServer.h).
It has no editor, JSON-RPC, authentication, session, or SSE protocol policy.
The MCP adapter supplies those policies through filters, routes, and providers.
Only the backend source includes socket/platform and third-party HTTP headers.

## Requests and Responses

`FHTTPServer::Start` takes transport options, exact-path GET/POST/DELETE routes,
an optional request filter, and an optional application exception handler. A
filter returning true completes the request before routing, including requests
for unknown paths. Header lookup is case-insensitive; duplicate counts are
preserved. Request body and method/path access do not copy the underlying bytes.
Port zero selects a free port; Start returns the bound port after both the
worker pool and listener are ready. Thread startup failures return an error and
release the bound port and any workers already created.
`GetLocalPort` supplies the actual receiving endpoint to request policies.

Handlers, filters, and stream providers run on request workers and may overlap
across requests. Request/response views exist only during their callback; retain
owned application data rather than those views. The exception handler translates
application handler/filter exceptions to a response and must not throw.

Chunk providers receive the byte offset and a borrowed sink. Write returns false
on failure; a provider returns false to abort or calls Done to finish successfully.
Body setters replace any previous body and its content/framing headers. Replacing
a stream retires its provider and calls its completion callback with false before
releasing provider captures. A handler/filter exception likewise retires any body
before application exception translation, including when no exception handler
is installed. Other response headers remain available to the error response.

The stream completion callback runs once after provider use ends, on replacement
or response release, including disconnect and shutdown. Its captures must outlive
that point, and it must not throw. Provider exceptions abort the connection and
complete the stream with false. Providers must return cooperatively and check
writability; HTTPServer cannot interrupt arbitrary application code blocked in a provider.
Disconnect is a transport observation; application cancellation remains the
consumer's decision.

## Resources and Lifetime

Each instance owns its listener, worker pool, queued connections, and accepted
sockets. Options configure the bind address, port, worker and queue bounds, body
limit, idle read/write timeouts, and keep-alive request count. Defaults are
loopback, 8 workers, 8 queued connections, a 1 MiB body, 2-second idle timeouts,
and one request per connection. TLS and compression backends are disabled.
Endpoints use exclusive Windows binding and address reuse without port reuse
on other hosts. Queue exhaustion closes excess connections.

The caller serializes Start, Stop, and destruction outside callbacks. Stop is
idempotent: it closes the listener, interrupts accepted sockets (including
partial requests), joins the listener and workers, and releases callback captures
before returning. Descriptor tracking and closure share a lock to prevent closing
a descriptor reused by another connection. Restart creates a fresh backend.

When the module is selected, `HTTPServerLifecycleTests` exercises the public API
with Core and HTTPServer as its only engine dependencies. MCP transport behavior remains covered by
`MCPIntegrationTests` and its official SDK smoke harness.
