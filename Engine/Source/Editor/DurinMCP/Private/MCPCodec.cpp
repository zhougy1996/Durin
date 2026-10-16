#include "MCPCodec.h"

namespace Durin::MCP
{
	namespace
	{
		auto Write(FJsonNodeRef Node, const FToolValue& Value) -> void
		{
			std::visit([&](const auto& Item) {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::same_as<T, std::monostate>) Node.SetValue(nullptr);
				else if constexpr (std::same_as<T, FObject>)
				{
					Node.EnsureObject();
					for (const auto& [Key, Child] : Item) Write(Node.AddObject(Key), Child);
				}
				else if constexpr (std::same_as<T, FArray>)
				{
					Node.EnsureArray();
					for (const auto& Child : Item) Write(Node.AppendObject(), Child);
				}
				else Node.SetValue(Item);
			}, Value.Data);
		}
		struct FBudget { size_t Nodes = 0; size_t Strings = 0; };
		auto Read(FJsonNodeView Node, FBudget& Budget, size_t Depth) -> std::expected<FToolValue, std::string>
		{
			if (Depth > 40 || ++Budget.Nodes > 8192 || Node.Num() > 2048)
				return std::unexpected("JSON tree exceeds protocol limits.");
			auto Charge = [&](std::string_view S) {
				Budget.Strings += S.size(); return S.size() <= 128 * 1024 && Budget.Strings <= MaxBodyBytes;
			};
			if (Node.IsObject())
			{
				FObject Object;
				std::optional<std::string> Failure;
				Node.ForEachObjectMember([&](std::string_view Key, FJsonNodeView Child) {
					if (Failure) return;
					if (!Charge(Key)) { Failure = "JSON key exceeds protocol limits."; return; }
					auto Value = Read(Child, Budget, Depth + 1);
					if (!Value) { Failure = Value.error(); return; }
					if (!Object.emplace(Key, std::move(*Value)).second) Failure = "Duplicate JSON object key.";
				});
				if (Failure) return std::unexpected(*Failure);
				return FToolValue(std::move(Object));
			}
			if (Node.IsArray())
			{
				FArray Array;
				for (size_t I = 0; I < Node.Num(); ++I)
				{
					auto Child = Read(Node.GetView(I), Budget, Depth + 1);
					if (!Child) return std::unexpected(Child.error());
					Array.push_back(std::move(*Child));
				}
				return FToolValue(std::move(Array));
			}
			if (Node.IsString())
			{
				auto S = Node.GetString();
				if (!Charge(S)) return std::unexpected("JSON string exceeds protocol limits.");
				return FToolValue(std::move(S));
			}
			if (Node.IsBool()) return FToolValue(Node.GetBool());
			if (Node.IsNull()) return FToolValue{};
			if (Node.IsUInt()) return FToolValue(Node.GetUInt());
			if (Node.IsInt()) return FToolValue(Node.GetInt());
			if (Node.IsNumber() && std::isfinite(Node.GetDouble())) return FToolValue(Node.GetDouble());
			return std::unexpected("Invalid JSON value.");
		}
		auto ToolName(const FToolAddress& Address) -> std::string
		{
			auto Escape = [](std::string_view Part) {
				std::string Result;
				for (char C : Part) { if (C == '_') Result += "_u"; else Result += C; }
				return Result;
			};
			auto Full = Escape(Address.Toolset) + "__" + Escape(Address.Tool);
			if (Full.size() <= 128) return Full;
			return Full.substr(0, 92) + "__h" + FXxHash128::HashBuffer(Full).ToString();
		}
	}

	auto Find(const FToolValue& Value, std::string_view Key) -> const FToolValue&
	{
		static const FToolValue Missing;
		if (const auto* Object = std::get_if<FObject>(&Value.Data))
			if (auto It = Object->find(Key); It != Object->end()) return It->second;
		return Missing;
	}
	auto Text(const FToolValue& Value) -> std::string_view
	{
		if (const auto* S = std::get_if<std::string>(&Value.Data)) return *S;
		return {};
	}
	auto ParseRequest(std::string_view TextValue) -> std::expected<FRequest, FRequestError>
	{
		if (TextValue.size() > MaxBodyBytes) return std::unexpected(FRequestError{ERequestErrorCode::InvalidRequest, "Request body exceeds 1 MiB."});
		// Bound nesting before the parser allocates its tree; escaped quotes do not toggle strings.
		size_t Depth = 0;
		bool String = false, Escape = false;
		for (char C : TextValue)
		{
			if (String) { if (Escape) Escape = false; else if (C == '\\') Escape = true; else if (C == '"') String = false; }
			else if (C == '"') String = true;
			else if (C == '{' || C == '[') { if (++Depth > 40) return std::unexpected(FRequestError{ERequestErrorCode::InvalidRequest, "JSON nesting exceeds protocol limits."}); }
			else if (C == '}' || C == ']') { if (Depth == 0) return std::unexpected(FRequestError{ERequestErrorCode::ParseError, "Invalid JSON nesting."}); --Depth; }
		}
		FJsonDocument Document;
		if (!Document.Parse(TextValue)) return std::unexpected(FRequestError{ERequestErrorCode::ParseError, "Malformed UTF-8 JSON request."});
		FBudget Budget;
		auto Root = Read(Document.GetRootView(), Budget, 0);
		if (!Root) return std::unexpected(FRequestError{ERequestErrorCode::InvalidRequest, Root.error()});
		const auto& Id = Find(*Root, "id");
		const bool Notification = std::holds_alternative<FObject>(Root->Data) && !std::get<FObject>(Root->Data).contains("id");
		if (Text(Find(*Root, "jsonrpc")) != "2.0" || Text(Find(*Root, "method")).empty()
			|| !(Notification || std::holds_alternative<std::string>(Id.Data) || std::holds_alternative<int64>(Id.Data)
				|| std::holds_alternative<uint64>(Id.Data)))
			return std::unexpected(FRequestError{ERequestErrorCode::InvalidRequest, "Expected a JSON-RPC request with a string or integer id."});
		auto Params = Find(*Root, "params");
		if (!std::get<FObject>(Root->Data).contains("params")) Params = FObject{};
		if (!std::holds_alternative<FObject>(Params.Data)) return std::unexpected(FRequestError{ERequestErrorCode::InvalidRequest, "Expected object params."});
		return FRequest{Id, std::string(Text(Find(*Root, "method"))), std::move(Params), Notification};
	}
	auto Serialize(const FToolValue& Value) -> std::string
	{
		FJsonDocument Document;
		Write(Document.GetMutableRoot(), Value);
		return Document.ToString(EJsonFormat::Compact);
	}
	auto Schema(const FToolSchema& Native) -> FToolValue
	{
		static constexpr const char* Types[]{"null", "boolean", "integer", "integer", "number", "string", "array", "object"};
		FObject Result{{"type", Types[static_cast<size_t>(Native.Type)]}};
		if (!Native.Description.empty()) Result.emplace("description", Native.Description);
		if (Native.Default) Result.emplace("default", *Native.Default);
		if (!Native.Enum.empty()) Result.emplace("enum", Native.Enum);
		if (Native.Minimum) Result.emplace("minimum", *Native.Minimum);
		if (Native.Maximum) Result.emplace("maximum", *Native.Maximum);
		if (Native.Type == EToolValueType::UnsignedInteger && !Native.Minimum) Result.emplace("minimum", uint64{0});
		if (Native.Type == EToolValueType::UnsignedInteger && !Native.Maximum) Result.emplace("maximum", UINT64_MAX);
		if (Native.Type == EToolValueType::Integer)
		{
			if (!Native.Minimum) Result.emplace("minimum", INT64_MIN);
			if (!Native.Maximum) Result.emplace("maximum", INT64_MAX);
		}
		if (Native.Type == EToolValueType::Object)
		{
			FObject Properties;
			for (const auto& [Key, Child] : Native.Properties) Properties.emplace(Key, Schema(Child));
			Result.emplace("properties", std::move(Properties));
			Result.emplace("additionalProperties", false);
			if (!Native.Required.empty())
			{
				FArray Required; for (const auto& Key : Native.Required) Required.emplace_back(Key);
				Result.emplace("required", std::move(Required));
			}
		}
		if (Native.Items) Result.emplace("items", Schema(*Native.Items));
		const auto MinKey = Native.Type == EToolValueType::String ? "minLength" : Native.Type == EToolValueType::Object ? "minProperties" : "minItems";
		const auto MaxKey = Native.Type == EToolValueType::String ? "maxLength" : Native.Type == EToolValueType::Object ? "maxProperties" : "maxItems";
		// Core string sizes count UTF-8 bytes; JSON Schema lengths count code points.
		// Describe byte bounds explicitly instead of advertising incompatible length constraints.
		if (Native.Type == EToolValueType::String)
		{
			if (Native.MinSize) Result.emplace("x-durin-minUtf8Bytes", *Native.MinSize);
			if (Native.MaxSize) Result.emplace("x-durin-maxUtf8Bytes", *Native.MaxSize);
		}
		else
		{
			if (Native.MinSize) Result.emplace(MinKey, *Native.MinSize);
			if (Native.MaxSize) Result.emplace(MaxKey, *Native.MaxSize);
		}
		return Result;
	}
	auto BuildCatalog(const FEditorToolCatalog& Native) -> std::expected<FCatalog, std::string>
	{
		FCatalog Result;
		for (const auto& Group : Native.Toolsets) for (const auto& Tool : Group.Descriptor.Tools)
		{
			if (!Tool.bExposeExternally) continue;
			FToolAddress Address{Group.Descriptor.Name, Tool.Name};
			auto Name = ToolName(Address);
			FObject Description{{"name", Name}, {"description", Tool.Description}, {"inputSchema", Schema(Tool.Input)},
				{"outputSchema", FObject{{"type", "object"}, {"properties", FObject{{"data", Schema(Tool.Output)}}},
					{"required", FArray{"data"}}, {"additionalProperties", false}}},
				{"annotations", FObject{{"readOnlyHint", Tool.Effects == EToolEffect::ReadOnly},
					{"destructiveHint", Tool.Effects != EToolEffect::ReadOnly},
					{"idempotentHint", Tool.RetryPolicy == EToolRetryPolicy::Idempotent},
					{"openWorldHint", (static_cast<uint8>(Tool.Effects) & static_cast<uint8>(EToolEffect::External)) != 0}}}};
			if (!Result.emplace(Name, FAdvertisedTool{std::move(Address), Tool.ContractVersion, Tool.Input, std::move(Description)}).second)
				return std::unexpected("MCP tool name collision; catalog rejected.");
		}
		return Result;
	}
	auto ConvertArguments(FToolValue Value, const FToolSchema& Native) -> FToolValue
	{
		if (Native.Type == EToolValueType::Integer)
		{
			if (const auto* U = std::get_if<uint64>(&Value.Data); U && *U <= static_cast<uint64>(INT64_MAX)) Value = static_cast<int64>(*U);
		}
		else if (Native.Type == EToolValueType::Number)
		{
			if (const auto* U = std::get_if<uint64>(&Value.Data)) Value = static_cast<double>(*U);
			else if (const auto* I = std::get_if<int64>(&Value.Data)) Value = static_cast<double>(*I);
		}
		else if (auto* Object = std::get_if<FObject>(&Value.Data); Object && Native.Type == EToolValueType::Object)
		{
			for (auto& [Key, Child] : *Object)
				if (auto It = Native.Properties.find(Key); It != Native.Properties.end()) Child = ConvertArguments(std::move(Child), It->second);
		}
		else if (auto* Array = std::get_if<FArray>(&Value.Data); Array && Native.Type == EToolValueType::Array && Native.Items)
			for (auto& Child : *Array) Child = ConvertArguments(std::move(Child), *Native.Items);
		return Value;
	}
	auto Reply(const FToolValue& Id, FObject Result) -> FToolValue
	{
		return FObject{{"jsonrpc", "2.0"}, {"id", Id}, {"result", std::move(Result)}};
	}
	auto Error(const FToolValue& Id, int Code, std::string Message, FToolValue Data) -> FToolValue
	{
		FObject Detail{{"code", Code}, {"message", std::move(Message)}};
		if (!std::holds_alternative<std::monostate>(Data.Data)) Detail.emplace("data", std::move(Data));
		return FObject{{"jsonrpc", "2.0"}, {"id", Id}, {"error", std::move(Detail)}};
	}
	auto ToolResult(const FToolValue& Id, const FToolResult& Result) -> FToolValue
	{
		FObject Fields{{"isError", Result.Status != EToolStatus::Succeeded}};
		FToolValue Payload;
		if (Result.Status == EToolStatus::Succeeded)
		{
			Payload = FObject{{"data", Result.Data}};
			Fields.emplace("structuredContent", Payload);
		}
		else
		{
			FArray Diagnostics;
			for (const auto& D : Result.Diagnostics) Diagnostics.emplace_back(FObject{{"code", static_cast<uint8>(D.Code)}, {"path", D.Path}, {"message", D.Message}});
			Payload = FObject{{"diagnostics", std::move(Diagnostics)}};
			Fields.emplace("_meta", FObject{{"durin/diagnostics", Find(Payload, "diagnostics")}});
		}
		auto Description = Serialize(Payload);
		if (Description.size() > 64 * 1024) Description = Result.Status == EToolStatus::Succeeded
			? "Structured tool result is available in structuredContent." : "Tool failed; full diagnostics are available in _meta.durin/diagnostics.";
		Fields.emplace("content", FArray{FObject{{"type", "text"}, {"text", std::move(Description)}}});
		return Reply(Id, std::move(Fields));
	}
	auto Initialize(const FToolValue& Id) -> FToolValue
	{
		return Reply(Id, {{"protocolVersion", ProtocolVersion}, {"capabilities", FObject{{"tools", FObject{}}}},
			{"serverInfo", FObject{{"name", "DurinMCP"}, {"version", "1.0.0"}}}});
	}
	auto List(const FToolValue& Id, const FCatalog& Catalog) -> FToolValue
	{
		FArray Tools;
		Tools.reserve(Catalog.size());
		for (const auto& [Name, Tool] : Catalog) Tools.push_back(Tool.Description);
		return Reply(Id, {{"tools", std::move(Tools)}});
	}
}
