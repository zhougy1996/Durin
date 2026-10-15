#include "Panels/ConsoleToolArguments.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/FilePath.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		auto Scalar(const FToolSchema& Schema, std::string_view Text) -> std::expected<FToolValue, std::string>
		{
			switch (Schema.Type)
			{
			case EToolValueType::String: return FToolValue(Text);
			case EToolValueType::Boolean:
				if (Text == "true") return FToolValue(true);
				if (Text == "false") return FToolValue(false);
				break;
			case EToolValueType::Integer:
			{
				int64 Value;
				auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
				if (Error == std::errc{} && End == Text.data() + Text.size()) return FToolValue(Value);
				break;
			}
			case EToolValueType::UnsignedInteger:
			{
				uint64 Value;
				auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
				if (Error == std::errc{} && End == Text.data() + Text.size()) return FToolValue(Value);
				break;
			}
			case EToolValueType::Number:
			{
				double Value;
				auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
				if (Error == std::errc{} && End == Text.data() + Text.size() && std::isfinite(Value)) return FToolValue(Value);
				break;
			}
			case EToolValueType::Null: if (Text == "null") return FToolValue{}; break;
			default: return std::unexpected("Objects and arrays require --args-file.");
			}
			return std::unexpected("Value does not match its declared scalar type or range.");
		}
		struct FJsonBudget { size_t Nodes = 0, StringBytes = 0; };
		auto ReadJson(const FJsonNodeView& Node, const FToolSchema& Schema, FJsonBudget& Budget, size_t Depth)
			-> std::expected<FToolValue, std::string>
		{
			const FToolLimits Limits;
			if (Depth > Limits.MaxDepth || ++Budget.Nodes > Limits.MaxNodes || Node.Num() > Limits.MaxContainerEntries)
				return std::unexpected("JSON argument tree exceeds its limits.");
			auto Charge = [&](std::string_view Text) {
				if (Text.size() > Limits.MaxStringBytes || Text.size() > Limits.MaxTotalStringBytes - Budget.StringBytes) return false;
				Budget.StringBytes += Text.size(); return true;
			};
			if (Schema.Type == EToolValueType::Object && Node.IsObject())
			{
				FToolValue::FObject Object;
				std::optional<std::string> Error;
				Node.ForEachObjectMember([&](std::string_view Key, FJsonNodeView Value) {
					if (Error) return;
					if (!Charge(Key)) { Error = "JSON object key exceeds its byte limit."; return; }
					const auto Field = Schema.Properties.find(Key);
					if (Field == Schema.Properties.end()) { Error = "Unknown JSON argument field."; return; }
					auto Child = ReadJson(Value, Field->second, Budget, Depth + 1);
					if (!Child) { Error = Child.error(); return; }
					if (!Object.emplace(std::string(Key), std::move(*Child)).second) Error = "Duplicate JSON argument field.";
				});
				if (Error) return std::unexpected(*Error);
				return FToolValue(std::move(Object));
			}
			if (Schema.Type == EToolValueType::Array && Node.IsArray() && Schema.Items)
			{
				FToolValue::FArray Array;
				for (size_t I = 0; I < Node.Num(); ++I)
				{
					auto Child = ReadJson(Node.GetView(I), *Schema.Items, Budget, Depth + 1);
					if (!Child) return std::unexpected(Child.error());
					Array.push_back(std::move(*Child));
				}
				return FToolValue(std::move(Array));
			}
			if (Schema.Type == EToolValueType::String && Node.IsString())
			{
				auto Text = Node.GetString();
				if (!Charge(Text)) return std::unexpected("JSON string exceeds its byte limit.");
				return FToolValue(std::move(Text));
			}
			if (Schema.Type == EToolValueType::Boolean && Node.IsBool()) return FToolValue(Node.GetBool());
			if (Schema.Type == EToolValueType::Null && Node.IsNull()) return FToolValue{};
			// yyjson represents nonnegative JSON integers as uint, even for signed schemas.
			if (Schema.Type == EToolValueType::UnsignedInteger && Node.IsUInt()) return FToolValue(Node.GetUInt());
			if (Schema.Type == EToolValueType::Integer)
			{
				if (Node.IsUInt() && Node.GetUInt() <= static_cast<uint64>(std::numeric_limits<int64>::max())) return FToolValue(static_cast<int64>(Node.GetUInt()));
				if (Node.IsInt() && !Node.IsUInt()) return FToolValue(Node.GetInt());
			}
			if (Schema.Type == EToolValueType::Number && Node.IsNumber() && std::isfinite(Node.GetDouble())) return FToolValue(Node.GetDouble());
			return std::unexpected("JSON argument does not match its declared type or integer range.");
		}
		auto WriteJson(FJsonNodeRef Node, const FToolValue& Value) -> void
		{
			std::visit([&](const auto& Item) {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::same_as<T, std::monostate>) Node.SetValue(nullptr);
				else if constexpr (std::same_as<T, FToolValue::FObject>)
				{
					Node.EnsureObject();
					for (const auto& [Key, Child] : Item) WriteJson(Node.AddObject(Key), Child);
				}
				else if constexpr (std::same_as<T, FToolValue::FArray>)
				{
					Node.EnsureArray();
					for (const auto& Child : Item) WriteJson(Node.AppendObject(), Child);
				}
				else if constexpr (std::same_as<T, std::string>) Node.SetValue(std::string_view(Item));
				else Node.SetValue(Item);
			}, Value.Data);
		}
	}

	auto TokenizeToolText(std::string_view Line) -> std::expected<std::vector<std::string>, std::string>
	{
		if (Line.size() > MaxConsoleToolTextBytes) return std::unexpected("Command text exceeds 1 MiB.");
		std::vector<std::string> Tokens;
		std::string Token;
		char Quote = 0;
		bool Started = false;
		for (size_t I = 0; I < Line.size(); ++I)
		{
			const char C = Line[I];
			if (Quote)
			{
				if (C == Quote) Quote = 0;
				else if (C == '\\' && I + 1 < Line.size() && (Line[I + 1] == Quote || Line[I + 1] == '\\')) Token += Line[++I];
				else Token += C;
			}
			else if (C == '\'' || C == '"') { Quote = C; Started = true; }
			else if (std::isspace(static_cast<unsigned char>(C)))
			{
				if (Started) { Tokens.push_back(std::move(Token)); Token.clear(); Started = false; }
			}
			else { Token += C; Started = true; }
		}
		if (Quote) return std::unexpected("Command contains an unterminated quote.");
		if (Started) Tokens.push_back(std::move(Token));
		return Tokens;
	}
	auto ParseConsoleToolArguments(const FToolDescriptor& Descriptor, std::span<const std::string> Tokens)
		-> std::expected<FToolValue, std::string>
	{
		if (!Tokens.empty() && Tokens.front() == "--args-file")
		{
			if (Tokens.size() != 2) return std::unexpected("Usage: <toolset>/<tool> --args-file <path>; do not mix field options.");
			auto Bytes = FFileHelper::LoadFileToArray(std::filesystem::u8path(Tokens[1]), {.MaximumBytes = MaxConsoleToolTextBytes});
			if (!Bytes) return std::unexpected(Bytes.error().ToString());
			std::string_view Text(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			if (Text.starts_with("\xef\xbb\xbf")) Text.remove_prefix(3);
			FJsonDocument Document;
			if (auto Parsed = Document.Parse(Text); !Parsed) return std::unexpected("Invalid UTF-8 JSON argument file: " + Parsed.error().Message);
			FJsonBudget Budget;
			return ReadJson(Document.GetRootView(), Descriptor.Input, Budget, 0);
		}
		FToolValue::FObject Object;
		if (Tokens.size() % 2 != 0) return std::unexpected("Expected --field <scalar> pairs.");
		for (size_t I = 0; I < Tokens.size(); I += 2)
		{
			if (!Tokens[I].starts_with("--") || Tokens[I] == "--args-file") return std::unexpected("Invalid field option or mixed --args-file input.");
			const std::string Name = Tokens[I].substr(2);
			const auto Field = Descriptor.Input.Properties.find(Name);
			if (Field == Descriptor.Input.Properties.end()) return std::unexpected("Unknown field option: " + Name);
			auto Value = Scalar(Field->second, Tokens[I + 1]);
			if (!Value) return std::unexpected(Name + ": " + Value.error());
			if (!Object.emplace(Name, std::move(*Value)).second) return std::unexpected("Duplicate field option: " + Name);
		}
		return FToolValue(std::move(Object));
	}
	auto FormatConsoleToolResult(const FToolResult& Result) -> FConsoleCommandResult
	{
		std::string Text;
		if (Result.Status == EToolStatus::Succeeded)
		{
			FJsonDocument Document; WriteJson(Document.GetMutableRoot(), Result.Data); Text = Document.ToString();
		}
		else for (const auto& Item : Result.Diagnostics)
		{
			if (!Text.empty()) Text += '\n';
			Text += std::format("Error {} {}: {}", static_cast<uint32>(Item.Code), Item.Path, Item.Message);
		}
		if (Text.size() > 64 * 1024) { Text.resize(64 * 1024); Text += "\n[Console display truncated]"; }
		return {Result.Status == EToolStatus::Succeeded, std::move(Text)};
	}
}
