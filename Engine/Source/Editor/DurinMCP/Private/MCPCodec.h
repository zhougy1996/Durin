#pragma once

#include "CoreMinimal.h"
#include "Json/Json.h"
#include "Tools/EditorToolService.h"

namespace Durin::MCP
{
	inline constexpr std::string_view ProtocolVersion = "2025-06-18";
	inline constexpr size_t MaxBodyBytes = 1024 * 1024;
	inline constexpr size_t MaxResponseBytes = 16 * 1024 * 1024;
	using FObject = FToolValue::FObject;
	using FArray = FToolValue::FArray;
	struct FAdvertisedTool
	{
		FToolAddress Address;
		uint32 ContractVersion;
		FToolSchema Input;
		FToolValue Description;
	};
	using FCatalog = std::map<std::string, FAdvertisedTool, std::less<>>;
	struct FRequest
	{
		FToolValue Id;
		std::string Method;
		FToolValue Params{FObject{}};
		bool bNotification = false;
	};
	enum class ERequestErrorCode { ParseError = -32700, InvalidRequest = -32600 };
	struct FRequestError
	{
		ERequestErrorCode Code;
		std::string Message;
	};
	// Detached protocol data only. No native provider callbacks run in this layer.
	auto ParseRequest(std::string_view Text) -> std::expected<FRequest, FRequestError>;
	auto Serialize(const FToolValue& Value) -> std::string;
	auto BuildCatalog(const FEditorToolCatalog& Native) -> std::expected<FCatalog, std::string>;
	auto Schema(const FToolSchema& Native) -> FToolValue;
	auto ConvertArguments(FToolValue Value, const FToolSchema& Native) -> FToolValue;
	auto Find(const FToolValue& Value, std::string_view Key) -> const FToolValue&;
	auto Text(const FToolValue& Value) -> std::string_view;
	auto Reply(const FToolValue& Id, FObject Result) -> FToolValue;
	auto Error(const FToolValue& Id, int Code, std::string Message, FToolValue Data = {}) -> FToolValue;
	auto ToolResult(const FToolValue& Id, const FToolResult& Result) -> FToolValue;
	auto Initialize(const FToolValue& Id) -> FToolValue;
	auto List(const FToolValue& Id, const FCatalog& Catalog) -> FToolValue;
}
