#include "DurinMCPModule.h"
#include "DurinPythonModule.h"

namespace Durin
{
	namespace
	{
		auto Environment(const char* Name) -> std::string
		{
#if defined(_WIN32)
			char* Buffer = nullptr; size_t Size = 0;
			if (_dupenv_s(&Buffer, &Size, Name) != 0) return {};
			std::unique_ptr<char, decltype(&std::free)> Owner(Buffer, &std::free);
			return Buffer ? std::string(Buffer) : std::string{};
#else
			const char* Value = std::getenv(Name);
			return Value ? Value : "";
#endif
		}
	}
	auto FDurinMCPModule::StartForEditor(DEditorEngine& Editor) -> void
	{
		const auto PortText = Environment("DURIN_MCP_PORT");
		if (PortText.empty()) return;
		uint16 Port = 0;
		auto [End, EC] = std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port);
		if (EC != std::errc{} || End != PortText.data() + PortText.size())
		{
			DURIN_WARN("MCP disabled: DURIN_MCP_PORT must be an integer from 0 to 65535."); return;
		}
		const auto TokenPath = std::filesystem::path(Environment("DURIN_MCP_TOKEN_FILE"));
		std::error_code Error;
		const auto Size = std::filesystem::file_size(TokenPath, Error);
		if (!TokenPath.is_absolute() || Error || Size > 256)
		{
			DURIN_WARN("MCP disabled: DURIN_MCP_TOKEN_FILE must name an absolute token file of at most 256 bytes."); return;
		}
		std::ifstream File(TokenPath, std::ios::binary);
		std::array<char, 257> Buffer{};
		File.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
		if (File.bad() || File.gcount() > 256)
		{
			DURIN_WARN("MCP disabled: token file could not be read within its byte limit."); return;
		}
		std::string Token(Buffer.data(), static_cast<size_t>(File.gcount()));
		while (!Token.empty() && (Token.back() == '\r' || Token.back() == '\n')) Token.pop_back();
		FMCPServerOptions Options{.Port = Port, .BearerToken = std::move(Token)};
		auto Python = FModuleManager::LoadModuleChecked<FDurinPythonModule>("DurinPython").EnsureRuntime();
		if (Python) Options.PythonRuntime = *Python;
		else DURIN_WARN("MCP Python tools unavailable: {}", Python.error());
		auto Started = Server.Start(Editor, Options);
		if (!Started) DURIN_WARN("MCP disabled: {}", Started.error());
		else DURIN_INFO("MCP listening at http://127.0.0.1:{}/mcp", *Started);
	}
	auto FDurinMCPModule::StopServer() noexcept -> void { Server.Stop(); }

	IMPLEMENT_MODULE(FDurinMCPModule, DurinMCP)
}
