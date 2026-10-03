#pragma once

#include "Misc/CoreTypes.h"
#include "Containers/ContainersFwd.h"

#include "Misc/CoreStd.h"

#include "CoreAPI.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	enum class ECbFieldType : uint8
	{
		Null = 0, Bool = 1, Int64 = 2, UInt64 = 3, Float32 = 4, Float64 = 5,
		String = 6, Binary = 7, ObjectId = 8, Array = 9, Object = 10,
	};

	struct FCbLimits
	{
		uint64 MaximumEncodedBytes = 4ull * 1024 * 1024;
		uint32 MaximumDepth = 32;
		uint32 MaximumFields = 4096;
		uint32 MaximumStringBytes = 1024 * 1024;
		uint32 MaximumBinaryBytes = 4 * 1024 * 1024;
	};

	struct FCbObjectId
	{
		std::array<uint8, 12> Bytes{};
		auto operator==(const FCbObjectId&) const -> bool = default;
		auto operator<=>(const FCbObjectId&) const = default;
	};

	class FCbFieldView;
	class FCbObjectView;
	class FCbArrayView;

	class FCbField
	{
	public:
		FCbField() = default;
		CORE_API static auto TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits = {})
			-> std::expected<FCbField, std::string>;
		auto IsValid() const -> bool { return !Bytes.IsEmpty(); }
		auto GetView() const -> FCbFieldView;
		auto GetBytes() const -> FSharedByteBuffer { return Bytes; }
	private:
		friend class FCbWriter;
		explicit FCbField(FSharedByteBuffer InBytes) : Bytes(std::move(InBytes)) {}
		FSharedByteBuffer Bytes;
	};

	class FCbObject
	{
	public:
		FCbObject() = default;
		CORE_API static auto TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits = {})
			-> std::expected<FCbObject, std::string>;
		auto IsValid() const -> bool { return Field.IsValid(); }
		auto GetView() const -> FCbObjectView;
		auto GetBytes() const -> FSharedByteBuffer { return Field.GetBytes(); }
	private:
		friend class FCbWriter;
		explicit FCbObject(FCbField InField) : Field(std::move(InField)) {}
		FCbField Field;
	};

	class FCbArray
	{
	public:
		FCbArray() = default;
		CORE_API static auto TryLoad(FSharedByteBuffer Bytes, FCbLimits Limits = {})
			-> std::expected<FCbArray, std::string>;
		auto IsValid() const -> bool { return Field.IsValid(); }
		auto GetView() const -> FCbArrayView;
		auto GetBytes() const -> FSharedByteBuffer { return Field.GetBytes(); }
	private:
		friend class FCbWriter;
		explicit FCbArray(FCbField InField) : Field(std::move(InField)) {}
		FCbField Field;
	};

	class FCbFieldView
	{
	public:
		FCbFieldView() = default;
		auto IsValid() const -> bool { return !Bytes.IsEmpty(); }
		auto GetBytes() const -> FSharedByteBuffer { return Bytes; }
		CORE_API auto GetType() const -> ECbFieldType;
		CORE_API auto GetName() const -> std::string_view;
		CORE_API auto AsBool() const -> std::optional<bool>;
		CORE_API auto AsInt64() const -> std::optional<int64>;
		CORE_API auto AsUInt64() const -> std::optional<uint64>;
		CORE_API auto AsFloat32() const -> std::optional<float>;
		CORE_API auto AsFloat64() const -> std::optional<double>;
		CORE_API auto AsString() const -> std::optional<std::string_view>;
		CORE_API auto AsBinary() const -> std::optional<FSharedByteBuffer>;
		CORE_API auto AsObjectId() const -> std::optional<FCbObjectId>;
		CORE_API auto AsObject() const -> FCbObjectView;
		CORE_API auto AsArray() const -> FCbArrayView;
	private:
		friend class FCbField;
		friend class FCbObjectView;
		friend class FCbArrayView;
		explicit FCbFieldView(FSharedByteBuffer InBytes) : Bytes(std::move(InBytes)) {}
		FSharedByteBuffer Bytes;
	};

	class FCbObjectView
	{
	public:
		FCbObjectView() = default;
		auto IsValid() const -> bool { return Field.IsValid() && Field.GetType() == ECbFieldType::Object; }
		CORE_API auto Num() const -> uint32;
		CORE_API auto Find(std::string_view Name) const -> FCbFieldView;
		CORE_API auto GetFields() const -> std::vector<FCbFieldView>;
	private:
		friend class FCbObject;
		friend class FCbFieldView;
		explicit FCbObjectView(FCbFieldView InField) : Field(std::move(InField)) {}
		FCbFieldView Field;
	};

	class FCbArrayView
	{
	public:
		FCbArrayView() = default;
		auto IsValid() const -> bool { return Field.IsValid() && Field.GetType() == ECbFieldType::Array; }
		CORE_API auto Num() const -> uint32;
		CORE_API auto At(uint32 Index) const -> FCbFieldView;
		CORE_API auto GetFields() const -> std::vector<FCbFieldView>;
	private:
		friend class FCbArray;
		friend class FCbFieldView;
		explicit FCbArrayView(FCbFieldView InField) : Field(std::move(InField)) {}
		FCbFieldView Field;
	};

	class FCbWriter
	{
	public:
		explicit FCbWriter(FCbLimits Limits = {}) : Limits(Limits) {}
		CORE_API auto AddNull(std::string Name = {}) -> bool;
		CORE_API auto AddBool(bool Value, std::string Name = {}) -> bool;
		CORE_API auto AddInt64(int64 Value, std::string Name = {}) -> bool;
		CORE_API auto AddUInt64(uint64 Value, std::string Name = {}) -> bool;
		CORE_API auto AddFloat32(float Value, std::string Name = {}) -> bool;
		CORE_API auto AddFloat64(double Value, std::string Name = {}) -> bool;
		CORE_API auto AddString(std::string Value, std::string Name = {}) -> bool;
		CORE_API auto AddBinary(FSharedByteBuffer Value, std::string Name = {}) -> bool;
		CORE_API auto AddObjectId(FCbObjectId Value, std::string Name = {}) -> bool;
		CORE_API auto AddObject(FCbObject Value, std::string Name = {}) -> bool;
		CORE_API auto AddArray(FCbArray Value, std::string Name = {}) -> bool;
		CORE_API auto SaveObject() -> std::expected<FCbObject, std::string>;
		CORE_API auto SaveArray() -> std::expected<FCbArray, std::string>;
	private:
		struct FEntry { std::string Name; FByteBuffer Bytes; };
		auto Add(ECbFieldType Type, FByteView Payload, std::string Name) -> bool;
		auto Save(bool Object) -> std::expected<FCbField, std::string>;
		FCbLimits Limits;
		std::vector<FEntry> Entries;
		std::string Error;
	};
}
