#pragma once

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	struct FYamlParseError
	{
		int32 Code = 0;
		size_t BytePosition = 0;
		size_t Line = 0;
		size_t Column = 0;
		std::string Message;
	};

	struct FYamlNodeAccess;

	class FYamlNodeView
	{
	public:
		FYamlNodeView() = default;

		CORE_API auto IsValid() const -> bool;
		CORE_API auto IsScalar() const -> bool;
		CORE_API auto IsMap() const -> bool;
		CORE_API auto IsSequence() const -> bool;
		CORE_API auto Num() const -> size_t;

		CORE_API auto Contains(std::string_view InKey) const -> bool;
		CORE_API auto GetKey() const -> std::string;

		// Returns an invalid view when this node is not a map or when the key is missing.
		CORE_API auto GetView(std::string_view InKey) const -> FYamlNodeView;
		// Returns an invalid view when this node is not a sequence/container or the index is out of range.
		CORE_API auto GetView(size_t Index) const -> FYamlNodeView;

		// Scalar getters return DefaultValue when the node is invalid, not scalar, or cannot be converted.
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

	protected:
		FYamlNodeView(void* InTreePtr, size_t InNodeIndex)
			: TreePtr(InTreePtr)
			, NodeIndex(InNodeIndex)
		{
		}

		void* TreePtr = nullptr;
		size_t NodeIndex = 0;

	private:
		static auto FromOpaque(void* InTreePtr, size_t InNodeIndex) -> FYamlNodeView
		{
			return FYamlNodeView(InTreePtr, InNodeIndex);
		}

		friend struct FYamlNodeAccess;
		friend class FYamlNodeRef;
		friend class FYamlDocument;
	};

	class FYamlNodeRef
		: public FYamlNodeView
	{
	public:
		FYamlNodeRef() = default;

		// Returns an invalid ref when this node is not a map or when the key is missing.
		CORE_API auto GetRef(std::string_view InKey) const -> FYamlNodeRef;
		// Returns an invalid ref when this node is not a sequence/container or the index is out of range.
		CORE_API auto GetRef(size_t Index) const -> FYamlNodeRef;

		// Converts this node into a map/sequence in place, clearing any previous scalar or container contents.
		CORE_API auto EnsureMap() -> FYamlNodeRef&;
		CORE_API auto EnsureSequence() -> FYamlNodeRef&;

		// Replaces this node with a scalar value.
		CORE_API auto SetString(std::string_view InValue) -> FYamlNodeRef&;
		CORE_API auto SetBool(bool bInValue) -> FYamlNodeRef&;
		CORE_API auto SetInt(int64 InValue) -> FYamlNodeRef&;
		CORE_API auto SetUInt(uint64 InValue) -> FYamlNodeRef&;
		CORE_API auto SetDouble(double InValue) -> FYamlNodeRef&;

		// Ensures this node is a map, then writes the scalar field at InKey.
		CORE_API auto SetStringValue(std::string_view InKey, std::string_view InValue) -> FYamlNodeRef&;
		CORE_API auto SetBoolValue(std::string_view InKey, bool bInValue) -> FYamlNodeRef&;
		CORE_API auto SetIntValue(std::string_view InKey, int64 InValue) -> FYamlNodeRef&;
		CORE_API auto SetUIntValue(std::string_view InKey, uint64 InValue) -> FYamlNodeRef&;
		CORE_API auto SetDoubleValue(std::string_view InKey, double InValue) -> FYamlNodeRef&;

		// Ensures this node is a map, then replaces any existing value at InKey with a new empty map/sequence.
		CORE_API auto AddMap(std::string_view InKey) -> FYamlNodeRef;
		CORE_API auto AddSequence(std::string_view InKey) -> FYamlNodeRef;

		// Ensures this node is a sequence, then appends a new scalar/container element.
		CORE_API auto AppendString(std::string_view InValue) -> FYamlNodeRef&;
		CORE_API auto AppendBool(bool bInValue) -> FYamlNodeRef&;
		CORE_API auto AppendInt(int64 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendUInt(uint64 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendDouble(double InValue) -> FYamlNodeRef&;

		CORE_API auto AppendMap() -> FYamlNodeRef;
		CORE_API auto AppendSequence() -> FYamlNodeRef;

	private:
		FYamlNodeRef(void* InTreePtr, size_t InNodeIndex)
			: FYamlNodeView(InTreePtr, InNodeIndex)
		{
		}

		static auto FromOpaque(void* InTreePtr, size_t InNodeIndex) -> FYamlNodeRef
		{
			return FYamlNodeRef(InTreePtr, InNodeIndex);
		}

		friend struct FYamlNodeAccess;
		friend class FYamlDocument;
	};

	class FYamlDocument
	{
	public:
		CORE_API FYamlDocument();
		CORE_API ~FYamlDocument();

		FYamlDocument(const FYamlDocument&) = delete;
		auto operator=(const FYamlDocument&) -> FYamlDocument& = delete;

		CORE_API FYamlDocument(FYamlDocument&& Other) noexcept;
		CORE_API auto operator=(FYamlDocument&& Other) noexcept -> FYamlDocument&;

		CORE_API auto Parse(std::string_view YamlText, FYamlParseError* OutError = nullptr) -> bool;
		CORE_API auto LoadFromFile(std::string_view FilePath, FYamlParseError* OutError = nullptr) -> bool;
		CORE_API auto Reset() -> void;

		CORE_API auto IsValid() const -> bool;
		CORE_API auto GetRootView() const -> FYamlNodeView;
		CORE_API auto GetMutableRoot() -> FYamlNodeRef;

		CORE_API auto ToString() const -> std::string;
		CORE_API auto SaveToFile(std::string_view FilePath) const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
