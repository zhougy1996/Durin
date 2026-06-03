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

	enum class EJsonNodeLink : uint8
	{
		None = 0,
		ObjectKey,
		ArrayIndex
	};

	class FJsonNodeView
	{
	public:
		FJsonNodeView() = default;

		CORE_API auto IsValid() const -> bool;
		CORE_API auto IsNull() const -> bool;
		CORE_API auto IsObject() const -> bool;
		CORE_API auto IsArray() const -> bool;
		CORE_API auto IsString() const -> bool;
		CORE_API auto IsBool() const -> bool;
		CORE_API auto IsInt() const -> bool;
		CORE_API auto IsUInt() const -> bool;
		CORE_API auto IsNumber() const -> bool;

		// Returns 0 when this node is not an object/array.
		CORE_API auto Num() const -> size_t;
		CORE_API auto Contains(std::string_view InKey) const -> bool;

		// Returns an invalid view when this node is not an object or when the key is missing.
		CORE_API auto GetView(std::string_view InKey) const -> FJsonNodeView;
		// Returns an invalid view when this node is not an array or the index is out of range.
		CORE_API auto GetView(size_t Index) const -> FJsonNodeView;

		// Scalar getters return DefaultValue when the node is invalid or of the wrong type.
		CORE_API auto GetString(std::string DefaultValue = "") const -> std::string;
		CORE_API auto GetBool(bool DefaultValue = false) const -> bool;
		CORE_API auto GetInt(int64 DefaultValue = 0) const -> int64;
		CORE_API auto GetUInt(uint64 DefaultValue = 0) const -> uint64;
		CORE_API auto GetDouble(double DefaultValue = 0.0) const -> double;

		// Convenience: GetView(InKey).Get*(DefaultValue).
		CORE_API auto GetStringValue(std::string_view InKey, std::string DefaultValue = "") const -> std::string;
		CORE_API auto GetBoolValue(std::string_view InKey, bool DefaultValue = false) const -> bool;
		CORE_API auto GetIntValue(std::string_view InKey, int64 DefaultValue = 0) const -> int64;
		CORE_API auto GetUIntValue(std::string_view InKey, uint64 DefaultValue = 0) const -> uint64;
		CORE_API auto GetDoubleValue(std::string_view InKey, double DefaultValue = 0.0) const -> double;

	protected:
		FJsonNodeView(void* InDocumentPtr, void* InValuePtr, bool bInIsMutable)
			: DocumentPtr(InDocumentPtr)
			, ValuePtr(InValuePtr)
			, bIsMutable(bInIsMutable)
		{
		}

		void* DocumentPtr = nullptr;
		void* ValuePtr = nullptr;
		bool bIsMutable = false;

	private:
		static auto FromOpaque(void* InDocumentPtr, void* InValuePtr, bool bInIsMutable) -> FJsonNodeView
		{
			return FJsonNodeView(InDocumentPtr, InValuePtr, bInIsMutable);
		}

		friend class FJsonNodeRef;
		friend class FJsonDocument;
		friend struct FJsonNodeAccess;
	};

	class FJsonNodeRef
		: public FJsonNodeView
	{
	public:
		FJsonNodeRef() = default;

		// Returns an invalid ref when this node is not an object or when the key is missing.
		CORE_API auto GetRef(std::string_view InKey) const -> FJsonNodeRef;
		// Returns an invalid ref when this node is not an array or the index is out of range.
		CORE_API auto GetRef(size_t Index) const -> FJsonNodeRef;

		// Converts this node into an object/array in place, clearing any previous value.
		CORE_API auto EnsureObject() -> FJsonNodeRef&;
		CORE_API auto EnsureArray() -> FJsonNodeRef&;

		// Replaces this node with a scalar value.
		CORE_API auto SetNull() -> FJsonNodeRef&;
		CORE_API auto SetString(std::string_view InValue) -> FJsonNodeRef&;
		CORE_API auto SetBool(bool bInValue) -> FJsonNodeRef&;
		CORE_API auto SetInt(int64 InValue) -> FJsonNodeRef&;
		CORE_API auto SetUInt(uint64 InValue) -> FJsonNodeRef&;
		CORE_API auto SetDouble(double InValue) -> FJsonNodeRef&;

		// Ensures this node is an object, then writes the scalar field at InKey.
		CORE_API auto SetNullValue(std::string_view InKey) -> FJsonNodeRef&;
		CORE_API auto SetStringValue(std::string_view InKey, std::string_view InValue) -> FJsonNodeRef&;
		CORE_API auto SetBoolValue(std::string_view InKey, bool bInValue) -> FJsonNodeRef&;
		CORE_API auto SetIntValue(std::string_view InKey, int64 InValue) -> FJsonNodeRef&;
		CORE_API auto SetUIntValue(std::string_view InKey, uint64 InValue) -> FJsonNodeRef&;
		CORE_API auto SetDoubleValue(std::string_view InKey, double InValue) -> FJsonNodeRef&;

		// Ensures this node is an object, then replaces any existing value at InKey with a new empty object/array.
		CORE_API auto AddObject(std::string_view InKey) -> FJsonNodeRef;
		CORE_API auto AddArray(std::string_view InKey) -> FJsonNodeRef;

		// Ensures this node is an array, then appends a new scalar/container element.
		CORE_API auto AppendNull() -> FJsonNodeRef&;
		CORE_API auto AppendString(std::string_view InValue) -> FJsonNodeRef&;
		CORE_API auto AppendBool(bool bInValue) -> FJsonNodeRef&;
		CORE_API auto AppendInt(int64 InValue) -> FJsonNodeRef&;
		CORE_API auto AppendUInt(uint64 InValue) -> FJsonNodeRef&;
		CORE_API auto AppendDouble(double InValue) -> FJsonNodeRef&;

		CORE_API auto AppendObject() -> FJsonNodeRef;
		CORE_API auto AppendArray() -> FJsonNodeRef;

	private:
		CORE_API auto ReplaceWith(void* InNewValuePtr) -> void*;
		CORE_API auto SetObjectEntryInternal(std::string_view InKey, void* InValuePtr) -> void*;
		CORE_API auto AppendArrayEntryInternal(void* InValuePtr, size_t* OutIndex = nullptr) -> void*;

		FJsonNodeRef(
			void* InDocumentPtr,
			void* InValuePtr,
			void* InParentPtr,
			std::string InKey,
			size_t InIndex,
			bool bInIsRoot,
			EJsonNodeLink InLink
		)
			: FJsonNodeView(InDocumentPtr, InValuePtr, true)
			, ParentPtr(InParentPtr)
			, Key(std::move(InKey))
			, Index(InIndex)
			, bIsRoot(bInIsRoot)
			, Link(InLink)
		{
		}

		static auto FromOpaque(
			void* InDocumentPtr,
			void* InValuePtr,
			void* InParentPtr,
			std::string InKey,
			size_t InIndex,
			bool bInIsRoot,
			EJsonNodeLink InLink
		) -> FJsonNodeRef
		{
			return FJsonNodeRef(InDocumentPtr, InValuePtr, InParentPtr, std::move(InKey), InIndex, bInIsRoot, InLink);
		}

		void* ParentPtr = nullptr;
		std::string Key;
		size_t Index = static_cast<size_t>(-1);
		bool bIsRoot = false;
		EJsonNodeLink Link = EJsonNodeLink::None;

		friend class FJsonDocument;
		friend struct FJsonNodeAccess;
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
		CORE_API auto GetRootView() const -> FJsonNodeView;
		CORE_API auto GetMutableRoot() -> FJsonNodeRef;

		CORE_API auto ToString() const -> std::string;
		CORE_API auto SaveToFile(std::string_view FilePath) const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
