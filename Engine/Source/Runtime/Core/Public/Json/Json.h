#pragma once

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	struct FJsonParseError
	{
		int32 Code = 0;
		size_t BytePosition = 0;
		size_t Line = 0;
		size_t Column = 0;
		size_t Character = 0;
		std::string Message;
	};

	class FJsonValueView
	{
	public:
		FJsonValueView() = default;
		static auto FromOpaque(void* InValuePtr) -> FJsonValueView
		{
			return FJsonValueView(InValuePtr);
		}

		CORE_API auto IsValid() const -> bool;
		CORE_API auto IsNull() const -> bool;
		CORE_API auto IsObject() const -> bool;
		CORE_API auto IsArray() const -> bool;
		CORE_API auto IsString() const -> bool;
		CORE_API auto IsBool() const -> bool;
		CORE_API auto IsInt() const -> bool;
		CORE_API auto IsUInt() const -> bool;
		CORE_API auto IsNumber() const -> bool;

		CORE_API auto Num() const -> size_t;

		CORE_API auto GetView(std::string_view InKey) const -> FJsonValueView;
		CORE_API auto GetView(size_t Index) const -> FJsonValueView;

		CORE_API auto GetString(std::string DefaultValue = "") const -> std::string;
		CORE_API auto GetBool(bool DefaultValue = false) const -> bool;
		CORE_API auto GetInt(int64 DefaultValue = 0) const -> int64;
		CORE_API auto GetUInt(uint64 DefaultValue = 0) const -> uint64;
		CORE_API auto GetDouble(double DefaultValue = 0.0) const -> double;

		CORE_API auto GetStringValue(std::string_view InKey, std::string DefaultValue = "") const -> std::string;
		CORE_API auto GetBoolValue(std::string_view InKey, bool DefaultValue = false) const -> bool;
		CORE_API auto GetIntValue(std::string_view InKey, int64 DefaultValue = 0) const -> int64;
		CORE_API auto GetUIntValue(std::string_view InKey, uint64 DefaultValue = 0) const -> uint64;
		CORE_API auto GetDoubleValue(std::string_view InKey, double DefaultValue = 0.0) const -> double;

	private:
		explicit FJsonValueView(void* InValuePtr)
			: ValuePtr(InValuePtr)
		{
		}

		void* ValuePtr = nullptr;
	};

	class FJsonDocument
	{
	public:
		CORE_API FJsonDocument();
		CORE_API ~FJsonDocument();

		FJsonDocument(const FJsonDocument&) = delete;
		auto operator=(const FJsonDocument&) -> FJsonDocument& = delete;

		CORE_API FJsonDocument(FJsonDocument&& Other) noexcept;
		CORE_API auto operator=(FJsonDocument&& Other) noexcept -> FJsonDocument&;

		CORE_API auto Parse(std::string_view JsonText, FJsonParseError* OutError = nullptr) -> bool;
		CORE_API auto LoadFromFile(std::string_view FileName, FJsonParseError* OutError = nullptr) -> bool;
		CORE_API auto Reset() -> void;

		CORE_API auto IsValid() const -> bool;
		CORE_API auto GetRootView() const -> FJsonValueView;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Lightweight JSON builder backed by yyjson's mutable document API.
	// The root is always an object. Nested objects/arrays are managed via Begin / End pairs.
	class FJsonWriter
	{
	public:
		CORE_API FJsonWriter();
		CORE_API ~FJsonWriter();

		FJsonWriter(const FJsonWriter&) = delete;
		auto operator=(const FJsonWriter&) -> FJsonWriter& = delete;
		CORE_API FJsonWriter(FJsonWriter&& Other) noexcept;
		CORE_API auto operator=(FJsonWriter&& Other) noexcept -> FJsonWriter&;

		// --- Object field setters (active when current container is an object) ---
		CORE_API auto AddFieldUInt(std::string_view Key, uint64 Value) -> FJsonWriter&;
		CORE_API auto AddFieldString(std::string_view Key, std::string_view Value) -> FJsonWriter&;

		// Begin a nested object field. Returns *this; call EndNested() to return to the parent.
		CORE_API auto BeginObjectField(std::string_view Key) -> FJsonWriter&;
		// Begin a nested array field. Returns *this; call EndNested() to return to the parent.
		CORE_API auto BeginArrayField(std::string_view Key) -> FJsonWriter&;

		// --- Array element setters (active when current container is an array) ---
		CORE_API auto AddElementUInt(uint64 Value) -> FJsonWriter&;
		CORE_API auto AddElementString(std::string_view Value) -> FJsonWriter&;
		// Begin a new object as the next array element.
		CORE_API auto BeginElementObject() -> FJsonWriter&;
		// Begin a new array as the next array element.
		CORE_API auto BeginElementArray() -> FJsonWriter&;

		// Pop the current nesting level and return to the parent container.
		CORE_API auto EndNested() -> FJsonWriter&;

		// --- Serialization ---
		CORE_API auto ToString() const -> std::string;
		CORE_API auto SaveToFile(std::string_view FilePath) const -> bool;
		CORE_API auto IsValid() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
