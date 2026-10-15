#include "Panels/ConsoleToolAdapter.h"
#include "Panels/ConsoleToolArguments.h"
#include "Editor/EditorEngine.h"
#include "Misc/StringHelper.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		// Match legacy command-name escaping without touching the argument text.
		auto CommandName(std::string_view Line) -> std::string
		{
			std::string Name;
			char Quote = 0;
			bool Escaped = false, Started = false;
			for (char C : Line)
			{
				if (Escaped) { Name += C; Escaped = false; Started = true; }
				else if (C == '\\') { Escaped = true; Started = true; }
				else if (Quote) { if (C == Quote) Quote = 0; else Name += C; }
				else if (C == '\'' || C == '"') { Quote = C; Started = true; }
				else if (std::isspace(static_cast<unsigned char>(C))) { if (Started) break; }
				else { Name += C; Started = true; }
			}
			return Name;
		}
		auto ReceiptName(FEditorToolInvocationId Id) -> std::string { return std::format("{}:{}", Id.Session, Id.Sequence); }
	}
	FConsoleToolAdapter::FConsoleToolAdapter(DEditorEngine& Editor, FConsoleCommandRegistry& InLegacy) : Host(&Editor), Legacy(InLegacy)
	{
		require(IsInGameThread());
		if (auto* Tools = ResolveSubsystem()) Endpoint = Tools->GetService();
	}
	FConsoleToolAdapter::~FConsoleToolAdapter()
	{
		if (Endpoint) for (const auto& Item : Pending) (void)Endpoint->Cancel(Item);
	}
	auto FConsoleToolAdapter::ResolveSubsystem() const -> DEditorToolSubsystem*
	{
		auto* Editor = Host.Get();
		return Editor ? Editor->GetEditorSubsystem<DEditorToolSubsystem>() : nullptr;
	}
	auto FConsoleToolAdapter::Entries() -> std::vector<FEntry>
	{
		std::map<std::string, FEntry> Entries;
		auto Add = [&](FEntry Entry) {
			const auto Key = StringUtils::FoldAscii(Entry.Name);
			if (auto Found = Entries.find(Key); Found != Entries.end()) Found->second.Collision = true;
			else Entries.emplace(Key, std::move(Entry));
		};
		for (const auto& Command : Legacy.GetCommands()) Add({Command.Name, Command.Description, Command.Usage});
		FEditorToolCatalog Catalog;
		if (auto* Tools = ResolveSubsystem()) Catalog = Tools->ListToolsets();
		for (const auto& Set : Catalog.Toolsets) for (const auto& Tool : Set.Descriptor.Tools)
		{
			const auto Name = Set.Descriptor.Name + "/" + Tool.Name;
			std::string Usage = Name;
			for (const auto& [Field, Schema] : Tool.Input.Properties)
				if (Field != "args-file" && Schema.Type != EToolValueType::Object && Schema.Type != EToolValueType::Array)
				{
					Usage += " --" + Field + " <";
					Usage += Schema.Type == EToolValueType::Boolean ? "true|false" : Schema.Type == EToolValueType::String ? "string" : "scalar";
					if (!Schema.Enum.empty())
					{
						Usage += ":";
						for (const auto& Value : Schema.Enum) if (const auto* Text = std::get_if<std::string>(&Value.Data)) Usage += *Text + "|";
					}
					Usage += Schema.Default ? "> (default)" : ">";
				}
			Usage += "\n   or: " + Name + " --args-file <path>";
			Add({Name, Tool.Description, std::move(Usage), FToolAddress{Set.Descriptor.Name, Tool.Name}});
		}
		std::vector<FEntry> Result;
		for (auto& [Name, Entry] : Entries) Result.push_back(std::move(Entry));
		return Result;
	}
	auto FConsoleToolAdapter::FindCompletions(std::string_view Prefix) -> std::vector<std::string>
	{
		require(IsInGameThread());
		std::vector<std::string> Result;
		const auto Key = StringUtils::FoldAscii(Prefix);
		for (const auto& Entry : Entries()) if (StringUtils::FoldAscii(Entry.Name).starts_with(Key)) Result.push_back(Entry.Name);
		return Result;
	}
	auto FConsoleToolAdapter::Execute(std::string_view Line) -> FConsoleCommandResult
	{
		try { return ExecuteInternal(Line); }
		catch (const std::exception& Error) { return FConsoleCommandResult::Failure(std::string("Console dispatch failed: ") + std::string(Error.what()).substr(0, 4096)); }
		catch (...) { return FConsoleCommandResult::Failure("Console dispatch failed with an unknown error."); }
	}
	auto FConsoleToolAdapter::ExecuteInternal(std::string_view Line) -> FConsoleCommandResult
	{
		if (!IsInGameThread()) return FConsoleCommandResult::Failure("Console dispatch requires GameThread.");
		if (Line.size() > MaxConsoleToolTextBytes) return FConsoleCommandResult::Failure("Command text exceeds 1 MiB.");
		const auto Name = CommandName(Line);
		if (Name.empty()) return Legacy.Execute(Line);
		auto Available = Entries();
		const auto Key = StringUtils::FoldAscii(Name);
		const auto Entry = std::ranges::find_if(Available, [&](const auto& Item) { return StringUtils::FoldAscii(Item.Name) == Key; });
		if (Entry != Available.end() && Entry->Collision) return FConsoleCommandResult::Failure("Command name collision: " + Name);
		if (Key == "help")
		{
			auto Tokens = TokenizeToolText(Line);
			if (!Tokens) return FConsoleCommandResult::Failure(Tokens.error());
			if (Tokens->size() > 2) return FConsoleCommandResult::Failure("Usage: help [command]");
			std::string Text = Tokens->size() == 1 ? "Available commands:" : "";
			for (const auto& Item : Available)
			{
				if (Tokens->size() == 2 && StringUtils::FoldAscii((*Tokens)[1]) != StringUtils::FoldAscii(Item.Name)) continue;
				Text += std::format("\n{}{}: {}", Item.Name, Item.Collision ? " [COLLISION]" : "", Item.Description);
				if (Tokens->size() == 2) Text += "\nUsage: " + Item.Usage;
			}
			if (Text.empty()) return FConsoleCommandResult::Failure("Unknown command '" + (*Tokens)[1] + "'.");
			return FConsoleCommandResult::Success(std::move(Text));
		}
		if (Entry == Available.end() || !Entry->Address) return Legacy.Execute(Line);
		if (Name != Entry->Name) return FConsoleCommandResult::Failure("Tool addresses are case-sensitive.");
		auto Tokens = TokenizeToolText(Line);
		if (!Tokens) return FConsoleCommandResult::Failure(Tokens.error());
		if (Tokens->empty() || Tokens->front() != Entry->Name)
			return FConsoleCommandResult::Failure("Write the command name exactly; tool addresses are case-sensitive.");
		auto* Tools = ResolveSubsystem();
		if (!Tools || !Endpoint) return FConsoleCommandResult::Failure("The editor tool service is unavailable.");
		FToolRequest Request{*Entry->Address, 1};
		auto Descriptor = Tools->DescribeTool(Request.Address);
		if (!Descriptor) return FConsoleCommandResult::Failure(Descriptor.error().Message);
		Request.ContractVersion = Descriptor->ContractVersion;
		auto Arguments = ParseConsoleToolArguments(*Descriptor, std::span<const std::string>(*Tokens).subspan(1));
		if (!Arguments) return FConsoleCommandResult::Failure(Arguments.error());
		Request.Arguments = std::move(*Arguments);
		if (Tools->CanCallNow()) return FormatConsoleToolResult(Tools->CallNow(Request, EToolSource::Console));
		if (Pending.size() >= 256) return FConsoleCommandResult::Failure("Console pending receipt capacity exceeded.");
		auto Submitted = Endpoint->Submit(Request, EToolSource::Console);
		if (!Submitted) return FConsoleCommandResult::Failure(Submitted.error().Message);
		Pending.push_back(*Submitted);
		return FConsoleCommandResult::Success("Queued invocation " + ReceiptName(*Submitted));
	}
	auto FConsoleToolAdapter::Poll() -> std::vector<FConsoleCommandResult>
	{
		require(IsInGameThread());
		std::vector<FConsoleCommandResult> Results;
		if (!Endpoint) return Results;
		std::erase_if(Pending, [&](const auto& Item) {
			auto Receipt = Endpoint->GetInvocation(Item);
			if (Receipt && (Receipt->State == EToolInvocationState::Queued || Receipt->State == EToolInvocationState::Running)) return false;
			auto Result = Receipt && Receipt->Result ? FormatConsoleToolResult(*Receipt->Result)
				: FConsoleCommandResult::Failure(Receipt ? "Invocation result expired." : Receipt.error().Message);
			Result.Message = "Invocation " + ReceiptName(Item) + ": " + Result.Message;
			Results.push_back(std::move(Result));
			return true;
		});
		return Results;
	}
}
