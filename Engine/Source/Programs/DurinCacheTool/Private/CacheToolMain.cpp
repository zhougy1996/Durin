#include "CoreMinimal.h"
#include "DerivedDataCache/DerivedDataCacheMaintenance.h"
#include "Json/Json.h"
#include <csignal>

namespace
{
	volatile std::sig_atomic_t Canceled = 0;
	auto Interrupt(int) -> void { Canceled = 1; }
}
int main(int Argc, char** Argv)
try
{
	using namespace Durin; using namespace Durin::DerivedData;
	FCacheMaintenanceOptions Options;
	if (Argc < 3) { std::cerr << "Usage: DurinCacheTool inspect|verify|clear --root=ABSOLUTE_PATH [--max-entries=N --max-file-bytes=N --max-read-bytes=N --max-working-bytes=N]\n"; return 2; }
	const std::string_view Operation = Argv[1];
	if (Operation == "verify") Options.Operation = ECacheMaintenanceOperation::Verify;
	else if (Operation == "clear") Options.Operation = ECacheMaintenanceOperation::Clear;
	else if (Operation != "inspect") { std::cerr << "Unknown maintenance operation.\n"; return 2; }
	for (int Index = 2; Index < Argc; ++Index)
	{
		const std::string_view Argument = Argv[Index]; const auto Separator = Argument.find('=');
		if (Separator == std::string_view::npos) { std::cerr << "Options require --name=value.\n"; return 2; }
		const auto Name = Argument.substr(0, Separator), Value = Argument.substr(Separator + 1);
		if (Name == "--root") { Options.Root = std::filesystem::path(Value); continue; }
		uint64 Number = 0; const auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), Number);
		if (Parsed.ec != std::errc{} || Parsed.ptr != Value.data() + Value.size() || !Number) { std::cerr << "Invalid numeric limit.\n"; return 2; }
		if (Name == "--max-entries" && Number <= 1000000) Options.MaximumEntries = uint32(Number);
		else if (Name == "--max-file-bytes") Options.MaximumFileBytes = Number;
		else if (Name == "--max-read-bytes") Options.MaximumReadBytes = Number;
		else if (Name == "--max-working-bytes") Options.MaximumWorkingSetBytes = Number;
		else { std::cerr << "Unknown option or invalid entry limit.\n"; return 2; }
	}
	std::signal(SIGINT, Interrupt); std::signal(SIGTERM, Interrupt);
	Options.ShouldCancel = [] { return Canceled != 0; };
	auto Report = MaintainCacheStorage(Options);
	FJsonDocument Json; auto Root = Json.GetMutableRoot();
	Root.SetChildValue("schemaVersion", 1u); Root.SetChildValue("operation", Operation);
	Root.SetChildValue("root", Options.Root.generic_string()); Root.SetChildValue("complete", bool(Report));
	if (!Report)
	{
		Root.SetChildValue("valid", false); Root.SetChildValue("diagnostic", Report.error().Diagnostic);
		Root.SetChildValue("errorCode", uint32(Report.error().Code));
	}
	else
	{
		Root.SetChildValue("valid", Report->IsValid());
		Root.SetChildValue("recordFiles", Report->RecordFiles); Root.SetChildValue("recordBytes", Report->RecordBytes);
		Root.SetChildValue("valueFiles", Report->ValueFiles); Root.SetChildValue("valueBytes", Report->ValueBytes);
		Root.SetChildValue("readBytes", Report->ReadBytes); Root.SetChildValue("verifiedFiles", Report->VerifiedFiles);
		Root.SetChildValue("invalidFiles", Report->InvalidFiles); Root.SetChildValue("unsupportedFiles", Report->UnsupportedFiles);
		Root.SetChildValue("missingValues", Report->MissingValues); Root.SetChildValue("removedFiles", Report->RemovedFiles);
		Root.SetChildValue("removedBytes", Report->RemovedBytes); Root.SetChildValue("otherRootEntries", Report->OtherRootEntries);
		auto Diagnostics = Root.AddArray("diagnostics"); for (const auto& Diagnostic : Report->Diagnostics) Diagnostics.AppendValue(Diagnostic);
	}
	std::cout << Json.ToString(EJsonFormat::Compact) << '\n';
	// The launcher maps report status to its exit code after retaining diagnostics.
	return 0;
}
catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
