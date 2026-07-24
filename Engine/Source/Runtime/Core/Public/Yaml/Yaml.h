#pragma once

#include "CoreAPI.h"
#include "CoreFwd.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	// Reports a YAML parser failure in both byte and source-text coordinates.
	struct FYamlParseError
	{
		int32 Code = 0;
		size_t BytePosition = 0;
		size_t Line = 0;
		size_t Column = 0;
		std::string Message;
	};

	struct FYamlNodeAccess;

	// Provides a non-owning read-only view whose lifetime is bounded by its document.
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

		CORE_API auto GetValue(std::string& OutValue) const -> bool;
		CORE_API auto GetValue(bool& bOutValue) const -> bool;
		CORE_API auto GetValue(int64& OutValue) const -> bool;
		CORE_API auto GetValue(uint64& OutValue) const -> bool;
		CORE_API auto GetValue(double& OutValue) const -> bool;

		CORE_API auto GetChildValue(std::string_view InKey, std::string& OutValue) const -> bool;
		CORE_API auto GetChildValue(std::string_view InKey, bool& bOutValue) const -> bool;
		CORE_API auto GetChildValue(std::string_view InKey, int64& OutValue) const -> bool;
		CORE_API auto GetChildValue(std::string_view InKey, uint64& OutValue) const -> bool;
		CORE_API auto GetChildValue(std::string_view InKey, double& OutValue) const -> bool;

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

	// Provides a non-owning mutable reference into a document-owned YAML tree.
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
		CORE_API auto SetValue(std::string_view InValue) -> void;
		CORE_API auto SetValue(const char* InValue) -> void;
		CORE_API auto SetValue(bool bInValue) -> void;
		CORE_API auto SetValue(int32 InValue) -> void;
		CORE_API auto SetValue(int64 InValue) -> void;
		CORE_API auto SetValue(uint32 InValue) -> void;
		CORE_API auto SetValue(uint64 InValue) -> void;
		CORE_API auto SetValue(double InValue) -> void;

		// Ensures this node is a map, then writes the scalar child at InKey.
		CORE_API auto SetChildValue(std::string_view InKey, std::string_view InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, const char* InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, bool bInValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, int32 InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, int64 InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, uint32 InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, uint64 InValue) -> void;
		CORE_API auto SetChildValue(std::string_view InKey, double InValue) -> void;

		// Ensures this node is a map, then replaces any existing value at InKey with a new empty map/sequence.
		CORE_API auto AddMap(std::string_view InKey) -> FYamlNodeRef;
		CORE_API auto AddSequence(std::string_view InKey) -> FYamlNodeRef;

		// Ensures this node is a sequence, then appends a new scalar/container element.
		CORE_API auto AppendValue(std::string_view InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(const char* InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(bool bInValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(int32 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(int64 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(uint32 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(uint64 InValue) -> FYamlNodeRef&;
		CORE_API auto AppendValue(double InValue) -> FYamlNodeRef&;

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

	// Owns a YAML tree and issues non-owning views into that tree.
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
