#pragma once

#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin::AssetPrivate
{
	inline constexpr uint64 MaximumPackageStringBytes = 1024 * 1024;

	struct FByteWriter
	{
		FByteArray Bytes;

		template<typename T> auto Write(const T& Value) -> void
		{
			const auto Data = std::as_bytes(std::span{&Value, 1});
			Bytes.insert(Bytes.end(), Data.begin(), Data.end());
		}

		auto WriteBytes(const void* Data, size_t Size) -> void
		{
			const auto* Source = static_cast<const std::byte*>(Data);
			Bytes.insert(Bytes.end(), Source, Source + Size);
		}

		auto WriteBytes(std::span<const std::byte> Value) -> void
		{
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}

		auto WriteString(std::string_view Value) -> void
		{
			Write(uint64(Value.size()));
			WriteBytes(Value.data(), Value.size());
		}
	};

	struct FByteReader
	{
		std::span<const std::byte> Bytes;
		size_t Offset = 0;

		template<typename T> auto Read(T& Value) -> bool
		{
			if (Offset > Bytes.size() || sizeof(T) > Bytes.size() - Offset) return false;
			std::memcpy(&Value, Bytes.data() + Offset, sizeof(T));
			Offset += sizeof(T);
			return true;
		}

		auto ReadBytes(void* Data, size_t Size) -> bool
		{
			if (Offset > Bytes.size() || Size > Bytes.size() - Offset) return false;
			std::memcpy(Data, Bytes.data() + Offset, Size);
			Offset += Size;
			return true;
		}

		auto ReadString(
			std::string& Value,
			uint64 MaximumSize = std::numeric_limits<uint64>::max()) -> bool
		{
			uint64 Size = 0;
			if (!Read(Size) || Size > MaximumSize || Size > Bytes.size() - Offset) return false;
			Value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), static_cast<size_t>(Size));
			Offset += static_cast<size_t>(Size);
			return true;
		}

		auto ReadSpan(size_t Size, std::span<const std::byte>& Out) -> bool
		{
			if (Offset > Bytes.size() || Size > Bytes.size() - Offset) return false;
			Out = Bytes.subspan(Offset, Size);
			Offset += Size;
			return true;
		}
	};

	inline auto GetSerializedTypeSignature(FProperty* Property) -> std::string
	{
		if (!Property) return "Invalid";
		const auto Kind = Property->GetKind();
		if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
			return std::format("Array<{}>", GetSerializedTypeSignature(
				static_cast<FArrayProperty*>(Property)->GetInner()));
		if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
		{
			auto* Map = static_cast<FMapProperty*>(Property);
			return std::format("Map<{},{}>", GetSerializedTypeSignature(Map->GetKeyProp()),
				GetSerializedTypeSignature(Map->GetValueProp()));
		}
		if (Kind == DurinCodeGen::EPropertyGenFlags::Object)
			return std::format("Object:{}:{}", Property->GetReferencedClass()
				? Property->GetReferencedClass()->GetQualifiedName().ToString() : "DObject",
				Property->IsObjectPtrWrapper());
		if (Kind == DurinCodeGen::EPropertyGenFlags::SoftObject)
		{
			auto* SoftObject = static_cast<FSoftObjectProperty*>(Property);
			return std::format("SoftObject:{}:v1", SoftObject->GetExpectedClass()
				? SoftObject->GetExpectedClass()->GetQualifiedName().ToString() : "DObject");
		}
		if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
		{
			auto* Enum = static_cast<FEnumProperty*>(Property);
			return std::format("Enum:{}:{}", Enum->GetEnum()
				? Enum->GetEnum()->GetQualifiedName().ToString() : "", Property->GetElementSize());
		}
		if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
		{
			auto* Struct = static_cast<FStructProperty*>(Property);
			return std::format("Struct<{}>", Struct->GetStruct()
				? Struct->GetStruct()->GetQualifiedName().ToString() : "");
		}
		if (Kind == DurinCodeGen::EPropertyGenFlags::String
			|| Kind == DurinCodeGen::EPropertyGenFlags::Name
			|| Kind == DurinCodeGen::EPropertyGenFlags::Guid
			|| Kind == DurinCodeGen::EPropertyGenFlags::Byte
			|| Kind == DurinCodeGen::EPropertyGenFlags::Blob
			|| Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
			return std::format("{}:v1", static_cast<uint32>(Kind));
		return std::format("{}:{}", static_cast<uint32>(Kind), Property->GetElementSize());
	}

	inline auto IsSerializedTypeSignatureCompatible(
		FProperty* Property, std::string_view Signature) -> bool
	{
		return Property && GetSerializedTypeSignature(Property) == Signature;
	}

	inline auto UnwrapFixed(const FArchiveLogicalTypeDescriptor& Type)
		-> const FArchiveLogicalTypeDescriptor&
	{
		return Type.Kind == FArchiveLogicalTypeDescriptor::EKind::FixedArray && Type.ElementType
			? *Type.ElementType : Type;
	}

	inline auto GetNativeKind(const FArchiveLogicalTypeDescriptor& Input)
		-> DurinCodeGen::EPropertyGenFlags
	{
		const auto& Type = UnwrapFixed(Input);
		using EKind = FArchiveLogicalTypeDescriptor::EKind;
		switch (Type.Kind)
		{
		case EKind::Scalar:
			if (Type.bFloating) return Type.BitWidth == 32
				? DurinCodeGen::EPropertyGenFlags::Float : DurinCodeGen::EPropertyGenFlags::Double;
			if (Type.bSigned)
			{
				switch (Type.BitWidth) { case 8: return DurinCodeGen::EPropertyGenFlags::Int8;
				case 16: return DurinCodeGen::EPropertyGenFlags::Int16;
				case 32: return DurinCodeGen::EPropertyGenFlags::Int32;
				default: return DurinCodeGen::EPropertyGenFlags::Int64; }
			}
			switch (Type.BitWidth) { case 8: return DurinCodeGen::EPropertyGenFlags::UInt8;
			case 16: return DurinCodeGen::EPropertyGenFlags::UInt16;
			case 32: return DurinCodeGen::EPropertyGenFlags::UInt32;
			default: return DurinCodeGen::EPropertyGenFlags::UInt64; }
		case EKind::Enum: return DurinCodeGen::EPropertyGenFlags::Enum;
		case EKind::String: return DurinCodeGen::EPropertyGenFlags::String;
		case EKind::Name: return DurinCodeGen::EPropertyGenFlags::Name;
		case EKind::Guid: return DurinCodeGen::EPropertyGenFlags::Guid;
		case EKind::Object: return DurinCodeGen::EPropertyGenFlags::Object;
		case EKind::SoftObject: return DurinCodeGen::EPropertyGenFlags::SoftObject;
		case EKind::WeakObject: return DurinCodeGen::EPropertyGenFlags::WeakObject;
		case EKind::Struct: return DurinCodeGen::EPropertyGenFlags::Struct;
		case EKind::Array: return DurinCodeGen::EPropertyGenFlags::Array;
		case EKind::Map: return DurinCodeGen::EPropertyGenFlags::Map;
		case EKind::Bytes: return DurinCodeGen::EPropertyGenFlags::Blob;
		case EKind::BulkData: return DurinCodeGen::EPropertyGenFlags::BulkData;
		case EKind::FixedArray: break;
		}
		return DurinCodeGen::EPropertyGenFlags::None;
	}

	inline auto GetNativeTypeSignature(const FArchiveLogicalTypeDescriptor& Input) -> std::string
	{
		const auto& Type = UnwrapFixed(Input);
		using EKind = FArchiveLogicalTypeDescriptor::EKind;
		if (Type.NativeFieldVersion != 0)
		{
			FArchiveLogicalTypeDescriptor Unversioned = Type;
			Unversioned.NativeFieldVersion = 0;
			return std::format("Native<{}>:v{}",
				GetNativeTypeSignature(Unversioned), Type.NativeFieldVersion);
		}
		switch (Type.Kind)
		{
		case EKind::Array:
			return std::format("Array<{}>", Type.ElementType
				? GetNativeTypeSignature(*Type.ElementType) : "Invalid");
		case EKind::Map:
			return std::format("Map<{},{}>", Type.KeyType
				? GetNativeTypeSignature(*Type.KeyType) : "Invalid", Type.ValueType
				? GetNativeTypeSignature(*Type.ValueType) : "Invalid");
		case EKind::Object:
			return std::format("Object:{}:true", Type.QualifiedType.IsNone()
				? "DObject" : Type.QualifiedType.ToString());
		case EKind::SoftObject:
			return std::format("SoftObject:{}:v1", Type.QualifiedType.IsNone()
				? "DObject" : Type.QualifiedType.ToString());
		case EKind::Enum:
			return std::format("Enum:{}:{}", Type.QualifiedType.ToString(), Type.BitWidth / 8);
		case EKind::Struct: return std::format("Struct<{}>", Type.QualifiedType.ToString());
		case EKind::String: return std::format("{}:v1", uint32(DurinCodeGen::EPropertyGenFlags::String));
		case EKind::Name: return std::format("{}:v1", uint32(DurinCodeGen::EPropertyGenFlags::Name));
		case EKind::Guid: return std::format("{}:v1", uint32(DurinCodeGen::EPropertyGenFlags::Guid));
		case EKind::Bytes: return std::format("{}:v1", uint32(DurinCodeGen::EPropertyGenFlags::Blob));
		case EKind::BulkData: return std::format("{}:v1", uint32(DurinCodeGen::EPropertyGenFlags::BulkData));
		default:
			return std::format("{}:{}", uint32(GetNativeKind(Type)), std::max<uint8>(1, Type.BitWidth / 8));
		}
	}
}
