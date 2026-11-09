#include "DurinMCPModule.h"
#include "DurinPythonModule.h"
#include "MCPServerSubsystem.h"
#include "PythonEditorSubsystem.h"

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
		if (BoundPort) return;
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
		if (!Python) DURIN_WARN("MCP Python tools unavailable: {}", Python.error());
		auto Started = Server.Start(Editor, Options);
		if (!Started) DURIN_WARN("MCP disabled: {}", Started.error());
		else { BoundPort = *Started; DURIN_INFO("MCP listening at http://127.0.0.1:{}/mcp", BoundPort); }
	}
	auto FDurinMCPModule::StopServer() noexcept -> void { Server.Stop(); BoundPort = 0; }

	auto FDurinMCPModule::StartupModule() -> void
	{
		FModuleManager::Get().LoadModuleChecked("DurinPython");
		EditorRegistration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = DMCPServerSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName(),
			.Dependencies = {DPythonEditorSubsystem::StaticClass()}});
	}
	auto FDurinMCPModule::CanShutdownModule() const -> bool
	{ return FEditorSubsystemCollection::CanSynchronizeActiveEditor(); }
	auto FDurinMCPModule::ShutdownModule() -> void
	{
		require(CanShutdownModule());
		EditorRegistration.reset();
		FEditorSubsystemCollection::SynchronizeActiveEditor();
		StopServer();
	}
	DMCPServerSubsystem::DMCPServerSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DMCPServerSubsystem::Initialize() -> FSubsystemResult
	{
		Module = static_cast<FDurinMCPModule*>(FModuleManager::Get().GetModule("DurinMCP"));
		check(Module);
		Module->StartForEditor(*GetEditor());
		return {};
	}
	auto DMCPServerSubsystem::Deinitialize() noexcept -> void
	{
		if (auto* Owner = std::exchange(Module, nullptr)) Owner->StopServer();
	}
	IMPLEMENT_MODULE(FDurinMCPModule, DurinMCP)
}
