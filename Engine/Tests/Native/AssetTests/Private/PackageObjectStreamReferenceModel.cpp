#include "PackageObjectStreamReferenceModel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Durin::Testing::PackageObjectStream
{
	namespace
	{
		auto Fail(std::string& OutError, std::string_view Message) -> bool
		{
			OutError = Message;
			return false;
		}

		auto ByteLess(std::string_view Left, std::string_view Right) -> bool
		{
			return std::lexicographical_compare(
				Left.begin(), Left.end(), Right.begin(), Right.end(),
				[](char A, char B) { return uint8(A) < uint8(B); });
		}

		auto ByteLess(std::span<const std::byte> Left, std::span<const std::byte> Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end());
		}

		auto IsKnownOpcode(ETypeOpcode Opcode) -> bool
		{
			return uint8(Opcode) >= uint8(ETypeOpcode::Bool)
				&& uint8(Opcode) <= uint8(ETypeOpcode::Bytes);
		}

		auto IsIntegerOpcode(ETypeOpcode Opcode) -> bool
		{
			return Opcode >= ETypeOpcode::I8 && Opcode <= ETypeOpcode::U64;
		}

		auto IsMapKeyOpcode(ETypeOpcode Opcode) -> bool
		{
			return Opcode == ETypeOpcode::Bool || IsIntegerOpcode(Opcode)
				|| Opcode == ETypeOpcode::String || Opcode == ETypeOpcode::Name
				|| Opcode == ETypeOpcode::Guid || Opcode == ETypeOpcode::Enum
				|| Opcode == ETypeOpcode::Intrinsic;
		}

		auto ValidateName(std::string_view Name, std::string& OutError) -> bool
		{
			if (Name.empty())
				return Fail(OutError, "name must be nonempty");
			if (Name.size() > MaximumStringBytes || !IsValidUtf8(Name))
				return Fail(OutError, "name is not bounded valid UTF-8");
			return true;
		}

		auto AppendStructuralKey(
			const FTypeDescriptor& Type,
			FWireWriter& Writer,
			std::unordered_set<const FTypeDescriptor*>& Visiting,
			std::string& OutError) -> bool
		{
			if (!Visiting.insert(&Type).second)
				return Fail(OutError, "type descriptor cycle");
			if (!IsKnownOpcode(Type.Opcode))
				return Fail(OutError, "unsupported type opcode");

			Writer.WriteU8(uint8(Type.Opcode));
			auto RequireShape = [&](uint64 Children, bool NameEmpty, uint64 Parameter) -> bool
			{
				if (Type.Children.size() != Children || Type.QualifiedName.empty() != NameEmpty
					|| Type.Parameter != Parameter)
					return Fail(OutError, "invalid type descriptor shape");
				return true;
			};

			bool Valid = true;
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool:
			case ETypeOpcode::I8:
			case ETypeOpcode::I16:
			case ETypeOpcode::I32:
			case ETypeOpcode::I64:
			case ETypeOpcode::U8:
			case ETypeOpcode::U16:
			case ETypeOpcode::U32:
			case ETypeOpcode::U64:
			case ETypeOpcode::F32:
			case ETypeOpcode::F64:
			case ETypeOpcode::String:
			case ETypeOpcode::Name:
			case ETypeOpcode::Guid:
			case ETypeOpcode::Bytes:
				Valid = RequireShape(0, true, 0);
				break;
			case ETypeOpcode::Enum:
				Valid = Type.Children.empty() && ValidateName(Type.QualifiedName, OutError)
					&& Type.Parameter >= uint8(ETypeOpcode::I8)
					&& Type.Parameter <= uint8(ETypeOpcode::U64);
				if (!Valid && OutError.empty())
					Fail(OutError, "invalid enum descriptor");
				if (Valid)
				{
					Writer.WriteString(Type.QualifiedName, OutError);
					Writer.WriteU8(uint8(Type.Parameter));
				}
				break;
			case ETypeOpcode::Intrinsic:
				Valid = Type.Children.empty() && Type.QualifiedName.empty()
					&& Type.Parameter >= 1 && Type.Parameter <= 6;
				if (!Valid)
					Fail(OutError, "invalid intrinsic descriptor");
				else
					Writer.WriteU8(uint8(Type.Parameter));
				break;
			case ETypeOpcode::Struct:
				Valid = Type.Children.empty() && Type.Parameter == 0
					&& ValidateName(Type.QualifiedName, OutError);
				if (Valid)
					Writer.WriteString(Type.QualifiedName, OutError);
				break;
			case ETypeOpcode::FixedArray:
				Valid = Type.Children.size() == 1 && Type.Children[0] && Type.QualifiedName.empty()
					&& Type.Parameter > 0 && Type.Parameter <= MaximumContainerElements;
				if (!Valid)
					Fail(OutError, "invalid fixed-array descriptor");
				break;
			case ETypeOpcode::Array:
			case ETypeOpcode::HardRef:
			case ETypeOpcode::SoftRef:
				if (Type.Opcode == ETypeOpcode::Array)
				{
					Valid = Type.Children.size() == 1 && Type.Children[0]
						&& Type.QualifiedName.empty() && Type.Parameter == 0;
					if (!Valid)
						Fail(OutError, "invalid array descriptor");
				}
				else
				{
					Valid = Type.Children.empty() && Type.Parameter == 0
						&& (Type.QualifiedName.empty() || ValidateName(Type.QualifiedName, OutError));
					if (Valid)
						Writer.WriteString(Type.QualifiedName, OutError);
				}
				break;
			case ETypeOpcode::Map:
				Valid = Type.Children.size() == 2 && Type.Children[0] && Type.Children[1]
					&& Type.QualifiedName.empty() && Type.Parameter == 0;
				if (!Valid)
					Fail(OutError, "invalid map descriptor");
				else if (!IsMapKeyOpcode(Type.Children[0]->Opcode))
				{
					Valid = false;
					Fail(OutError, "unsupported map key type");
				}
				break;
			}

			if (Valid && (Type.Opcode == ETypeOpcode::FixedArray
				|| Type.Opcode == ETypeOpcode::Array || Type.Opcode == ETypeOpcode::Map))
			{
				if (Type.Opcode == ETypeOpcode::FixedArray)
					Writer.WriteVarUInt(Type.Parameter);
				for (const FTypePtr& Child : Type.Children)
					if (!AppendStructuralKey(*Child, Writer, Visiting, OutError))
					{
						Valid = false;
						break;
					}
			}

			Visiting.erase(&Type);
			return Valid;
		}

		auto StructuralKey(const FTypeDescriptor& Type, std::vector<std::byte>& OutKey, std::string& OutError) -> bool
		{
			FWireWriter Writer;
			std::unordered_set<const FTypeDescriptor*> Visiting;
			if (!AppendStructuralKey(Type, Writer, Visiting, OutError))
				return false;
			OutKey = Writer.TakeBytes();
			return true;
		}

		auto AddName(std::vector<std::string>& Names, std::string_view Name, std::string& OutError) -> bool
		{
			if (!ValidateName(Name, OutError))
				return false;
			Names.emplace_back(Name);
			return true;
		}

		auto WriteRecord(FWireWriter& Writer, std::span<const std::byte> Record) -> void
		{
			Writer.WriteVarUInt(Record.size());
			Writer.WriteBytes(Record);
		}

		auto ReadRecord(FWireReader& Reader, std::span<const std::byte>& OutRecord, std::string& OutError) -> bool
		{
			uint64 Length = 0;
			return Reader.ReadVarUInt(Length, OutError) && Reader.ReadBytes(Length, OutRecord, OutError);
		}

		auto FindFieldType(
			const FFrozenTables& Tables,
			uint64 SchemaId,
			uint64 FieldId,
			const FTypeDescriptor*& OutType,
			std::string& OutError) -> bool
		{
			if (SchemaId == 0 || SchemaId > Tables.Schemas.size())
				return Fail(OutError, "schema id is out of range");
			const FSchemaDescriptor& Schema = Tables.Schemas[SchemaId - 1];
			if (FieldId == 0 || FieldId > Schema.Fields.size())
				return Fail(OutError, "field id is out of range");
			OutType = Schema.Fields[FieldId - 1].Type.get();
			return true;
		}
	}

	auto MakeType(
		ETypeOpcode Opcode,
		std::string QualifiedName,
		uint64 Parameter,
		std::vector<FTypePtr> Children) -> FTypePtr
	{
		return std::make_shared<FTypeDescriptor>(FTypeDescriptor{
			.Opcode = Opcode,
			.QualifiedName = std::move(QualifiedName),
			.Parameter = Parameter,
			.Children = std::move(Children),
		});
	}

	auto FFrozenTables::NameId(std::string_view Name) const -> uint64
	{
		const auto It = std::find(Names.begin(), Names.end(), Name);
		return It == Names.end() ? 0 : uint64(std::distance(Names.begin(), It) + 1);
	}

	auto FFrozenTables::TypeId(const FTypeDescriptor& Type) const -> uint64
	{
		std::vector<std::byte> Key;
		std::string Error;
		if (!StructuralKey(Type, Key, Error))
			return 0;
		const auto It = std::find_if(Types.begin(), Types.end(),
			[&](const FFrozenType& Candidate) { return Candidate.StructuralKey == Key; });
		return It == Types.end() ? 0 : uint64(std::distance(Types.begin(), It) + 1);
	}

	auto FFrozenTables::SchemaId(std::string_view Name) const -> uint64
	{
		const auto It = std::find_if(Schemas.begin(), Schemas.end(),
			[&](const FSchemaDescriptor& Schema) { return Schema.QualifiedName == Name; });
		return It == Schemas.end() ? 0 : uint64(std::distance(Schemas.begin(), It) + 1);
	}

	auto FFrozenTables::FieldId(uint64 InSchemaId, std::string_view Name) const -> uint64
	{
		if (InSchemaId == 0 || InSchemaId > Schemas.size())
			return 0;
		const auto& Fields = Schemas[InSchemaId - 1].Fields;
		const auto It = std::find_if(Fields.begin(), Fields.end(),
			[&](const FFieldDescriptor& Field) { return Field.Name == Name; });
		return It == Fields.end() ? 0 : uint64(std::distance(Fields.begin(), It) + 1);
	}

	auto FFrozenTables::ObjectId(std::string_view Path) const -> uint64
	{
		const auto It = std::find_if(Objects.begin(), Objects.end(),
			[&](const FObjectDescriptor& Object) { return Object.Path == Path; });
		return It == Objects.end() ? 0 : uint64(std::distance(Objects.begin(), It) + 1);
	}

	auto FDiscoveryRegistry::AddName(std::string Name, std::string& OutError) -> bool
	{
		if (Frozen)
			return Fail(OutError, "late discovery after freeze");
		if (!ValidateName(Name, OutError))
			return false;
		DiscoveredNames.insert(std::move(Name));
		return true;
	}

	auto FreezeTables(
		const FTableInput& Input,
		FFrozenTables& OutTables,
		std::string& OutError) -> bool
	{
		FFrozenTables Result;
		if (Input.PublicDependencyCount > MaximumDependencies)
			return Fail(OutError, "public dependency count exceeds bound");
		Result.PublicDependencyCount = Input.PublicDependencyCount;
		std::vector<std::string> Names;
		for (const std::string& Name : Input.AdditionalNames)
			if (!AddName(Names, Name, OutError))
				return false;

		std::vector<FTypePtr> DiscoveredTypes;
		std::unordered_set<const FTypeDescriptor*> WalkedTypes;
		std::function<bool(const FTypePtr&)> DiscoverType = [&](const FTypePtr& Type) -> bool
		{
			if (!Type)
				return Fail(OutError, "null type descriptor");
			std::vector<std::byte> Key;
			if (!StructuralKey(*Type, Key, OutError))
				return false;
			if (WalkedTypes.insert(Type.get()).second)
			{
				DiscoveredTypes.push_back(Type);
				if (!Type->QualifiedName.empty() && !AddName(Names, Type->QualifiedName, OutError))
					return false;
				for (const FTypePtr& Child : Type->Children)
					if (!DiscoverType(Child))
						return false;
			}
			return true;
		};

		for (const FTypePtr& Type : Input.Types)
			if (!DiscoverType(Type))
				return false;

		Result.Schemas = Input.Schemas;
		if (Result.Schemas.size() > MaximumTableEntries)
			return Fail(OutError, "schema count exceeds bound");
		for (FSchemaDescriptor& Schema : Result.Schemas)
		{
			if (!AddName(Names, Schema.QualifiedName, OutError))
				return false;
			if (Schema.Fields.size() > MaximumSchemaFields)
				return Fail(OutError, "schema field count exceeds bound");
			for (FFieldDescriptor& Field : Schema.Fields)
			{
				if (!AddName(Names, Field.Name, OutError) || !DiscoverType(Field.Type))
					return false;
				if (Field.AuthoredFlags != 0)
					return Fail(OutError, "unsupported authored field flags");
			}
			std::sort(Schema.Fields.begin(), Schema.Fields.end(),
				[&](const FFieldDescriptor& Left, const FFieldDescriptor& Right)
				{
					if (Left.Name != Right.Name)
						return ByteLess(Left.Name, Right.Name);
					std::vector<std::byte> LeftKey;
					std::vector<std::byte> RightKey;
					std::string Ignored;
					StructuralKey(*Left.Type, LeftKey, Ignored);
					StructuralKey(*Right.Type, RightKey, Ignored);
					if (LeftKey != RightKey)
						return ByteLess(LeftKey, RightKey);
					return Left.AuthoredFlags < Right.AuthoredFlags;
				});
			for (uint64 Index = 1; Index < Schema.Fields.size(); ++Index)
				if (Schema.Fields[Index - 1].Name == Schema.Fields[Index].Name)
					return Fail(OutError, "duplicate schema field");
		}
		std::sort(Result.Schemas.begin(), Result.Schemas.end(),
			[](const FSchemaDescriptor& Left, const FSchemaDescriptor& Right)
			{ return ByteLess(Left.QualifiedName, Right.QualifiedName); });
		for (uint64 Index = 1; Index < Result.Schemas.size(); ++Index)
			if (Result.Schemas[Index - 1].QualifiedName == Result.Schemas[Index].QualifiedName)
				return Fail(OutError, "duplicate schema");

		for (const FTypePtr& Type : DiscoveredTypes)
		{
			std::vector<std::byte> Key;
			if (!StructuralKey(*Type, Key, OutError))
				return false;
			const auto It = std::find_if(Result.Types.begin(), Result.Types.end(),
				[&](const FFrozenType& Existing) { return Existing.StructuralKey == Key; });
			if (It == Result.Types.end())
				Result.Types.push_back({Type, std::move(Key)});
		}
		if (Result.Types.size() > MaximumTableEntries)
			return Fail(OutError, "type count exceeds bound");
		std::sort(Result.Types.begin(), Result.Types.end(),
			[](const FFrozenType& Left, const FFrozenType& Right)
			{ return ByteLess(Left.StructuralKey, Right.StructuralKey); });

		Result.CustomVersions = Input.CustomVersions;
		if (Result.CustomVersions.size() > MaximumCustomVersions)
			return Fail(OutError, "custom version count exceeds bound");
		std::sort(Result.CustomVersions.begin(), Result.CustomVersions.end(),
			[](const FCustomVersion& Left, const FCustomVersion& Right)
			{ return Left.Guid < Right.Guid; });
		for (uint64 Index = 0; Index < Result.CustomVersions.size(); ++Index)
		{
			const FCustomVersion& Custom = Result.CustomVersions[Index];
			if (Index > 0 && Result.CustomVersions[Index - 1].Guid == Custom.Guid)
				return Fail(OutError, "duplicate custom version GUID");
			if (Custom.EmissionValue && *Custom.EmissionValue != Custom.Value)
				return Fail(OutError, "custom version discovery/emission mismatch");
			if (Custom.MaximumSupported && Custom.Value > *Custom.MaximumSupported)
				return Fail(OutError, "unsupported known custom version");
			if (Custom.RequiredForInterpretation && !Custom.CodecKnown)
				return Fail(OutError, "unknown required custom version");
		}

		Result.Objects = Input.Objects;
		if (Result.Objects.size() > MaximumObjects)
			return Fail(OutError, "object count exceeds bound");
		for (const FObjectDescriptor& Object : Result.Objects)
		{
			if (!AddName(Names, Object.ClassName, OutError) || !AddName(Names, Object.ObjectName, OutError)
				|| !ValidateName(Object.Path, OutError))
				return false;
		}
		std::sort(Result.Objects.begin(), Result.Objects.end(),
			[](const FObjectDescriptor& Left, const FObjectDescriptor& Right)
			{
				if (Left.OuterPath.empty() != Right.OuterPath.empty())
					return Left.OuterPath.empty();
				if (Left.OuterPath != Right.OuterPath)
					return ByteLess(Left.OuterPath, Right.OuterPath);
				if (Left.ClassName != Right.ClassName)
					return ByteLess(Left.ClassName, Right.ClassName);
				return ByteLess(Left.ObjectName, Right.ObjectName);
			});
		if (!Result.Objects.empty())
		{
			if (!Result.Objects[0].OuterPath.empty())
				return Fail(OutError, "package root is missing");
			for (uint64 Index = 0; Index < Result.Objects.size(); ++Index)
			{
				const FObjectDescriptor& Object = Result.Objects[Index];
				const std::string ExpectedPath = Object.OuterPath.empty()
					? Object.ObjectName : Object.OuterPath + "/" + Object.ObjectName;
				if (Object.Path != ExpectedPath)
					return Fail(OutError, "object canonical path mismatch");
				if (Index > 0 && Object.OuterPath.empty())
					return Fail(OutError, "multiple package roots");
				if (!Object.OuterPath.empty())
				{
					const auto Parent = std::find_if(Result.Objects.begin(), Result.Objects.begin() + Index,
						[&](const FObjectDescriptor& Candidate) { return Candidate.Path == Object.OuterPath; });
					if (Parent == Result.Objects.begin() + Index)
						return Fail(OutError, "object outer is missing or not canonical");
				}
				for (uint64 Other = 0; Other < Index; ++Other)
					if (Result.Objects[Other].Path == Object.Path)
						return Fail(OutError, "duplicate sibling object identity");
			}
		}

		std::sort(Names.begin(), Names.end(),
			[](const std::string& Left, const std::string& Right) { return ByteLess(Left, Right); });
		Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
		if (Names.size() > MaximumTableEntries)
			return Fail(OutError, "name count exceeds bound");
		Result.Names = std::move(Names);
		OutTables = std::move(Result);
		return true;
	}

	auto EncodeTableSections(
		const FFrozenTables& Tables,
		std::array<std::vector<std::byte>, 4>& OutSections,
		std::string& OutError) -> bool
	{
		std::array<std::vector<std::byte>, 4> Result;
		FWireWriter Names;
		Names.WriteVarUInt(Tables.Names.size());
		for (const std::string& Name : Tables.Names)
			if (!Names.WriteString(Name, OutError))
				return false;
		Result[0] = Names.TakeBytes();

		FWireWriter Types;
		Types.WriteVarUInt(Tables.Types.size());
		for (const FFrozenType& Frozen : Tables.Types)
		{
			const FTypeDescriptor& Type = *Frozen.Descriptor;
			FWireWriter Record;
			Record.WriteU8(uint8(Type.Opcode));
			switch (Type.Opcode)
			{
			case ETypeOpcode::Enum:
				Record.WriteVarUInt(Tables.NameId(Type.QualifiedName));
				Record.WriteU8(uint8(Type.Parameter));
				break;
			case ETypeOpcode::Intrinsic:
				Record.WriteU8(uint8(Type.Parameter));
				break;
			case ETypeOpcode::Struct:
				Record.WriteVarUInt(Tables.NameId(Type.QualifiedName));
				break;
			case ETypeOpcode::FixedArray:
				Record.WriteVarUInt(Tables.TypeId(*Type.Children[0]));
				Record.WriteVarUInt(Type.Parameter);
				break;
			case ETypeOpcode::Array:
				Record.WriteVarUInt(Tables.TypeId(*Type.Children[0]));
				break;
			case ETypeOpcode::Map:
				Record.WriteVarUInt(Tables.TypeId(*Type.Children[0]));
				Record.WriteVarUInt(Tables.TypeId(*Type.Children[1]));
				break;
			case ETypeOpcode::HardRef:
			case ETypeOpcode::SoftRef:
				Record.WriteVarUInt(Type.QualifiedName.empty() ? 0 : Tables.NameId(Type.QualifiedName));
				break;
			default:
				break;
			}
			WriteRecord(Types, Record.Bytes());
		}
		Result[1] = Types.TakeBytes();

		FWireWriter Schemas;
		Schemas.WriteVarUInt(Tables.CustomVersions.size());
		for (const FCustomVersion& Custom : Tables.CustomVersions)
		{
			Schemas.WriteU32(Custom.Guid.A);
			Schemas.WriteU32(Custom.Guid.B);
			Schemas.WriteU32(Custom.Guid.C);
			Schemas.WriteU32(Custom.Guid.D);
			Schemas.WriteVarUInt(Custom.Value);
		}
		Schemas.WriteVarUInt(Tables.Schemas.size());
		for (const FSchemaDescriptor& Schema : Tables.Schemas)
		{
			FWireWriter Record;
			Record.WriteVarUInt(Tables.NameId(Schema.QualifiedName));
			Record.WriteVarUInt(Schema.Fields.size());
			for (const FFieldDescriptor& Field : Schema.Fields)
			{
				Record.WriteVarUInt(Tables.NameId(Field.Name));
				Record.WriteVarUInt(Tables.TypeId(*Field.Type));
				Record.WriteVarUInt(Field.AuthoredFlags);
			}
			WriteRecord(Schemas, Record.Bytes());
		}
		Result[2] = Schemas.TakeBytes();

		FWireWriter Objects;
		Objects.WriteVarUInt(Tables.Objects.size());
		for (const FObjectDescriptor& Object : Tables.Objects)
		{
			FWireWriter Record;
			Record.WriteVarUInt(Object.OuterPath.empty() ? 0 : Tables.ObjectId(Object.OuterPath));
			Record.WriteVarUInt(Tables.NameId(Object.ClassName));
			Record.WriteVarUInt(Tables.NameId(Object.ObjectName));
			WriteRecord(Objects, Record.Bytes());
		}
		Result[3] = Objects.TakeBytes();
		OutSections = std::move(Result);
		return true;
	}

	auto DecodeTableSections(
		const std::array<std::vector<std::byte>, 4>& Sections,
		FFrozenTables& OutTables,
		std::string& OutError) -> bool
	{
		FTableInput Input;
		FWireReader NameReader(Sections[0]);
		uint64 NameCount = 0;
		if (!NameReader.ReadVarUInt(NameCount, OutError) || NameCount > MaximumTableEntries)
			return NameCount > MaximumTableEntries ? Fail(OutError, "name count exceeds bound") : false;
		Input.AdditionalNames.reserve(NameCount);
		for (uint64 Index = 0; Index < NameCount; ++Index)
		{
			std::string Name;
			if (!NameReader.ReadString(Name, OutError) || !ValidateName(Name, OutError))
				return false;
			if (!Input.AdditionalNames.empty()
				&& !ByteLess(Input.AdditionalNames.back(), Name))
				return Fail(OutError, "name table is not canonical");
			Input.AdditionalNames.push_back(std::move(Name));
		}
		if (!NameReader.RequireEnd(OutError))
			return false;

		struct FRawType
		{
			ETypeOpcode Opcode = ETypeOpcode::Bool;
			uint64 NameId = 0;
			uint64 Parameter = 0;
			std::vector<uint64> Children;
		};
		std::vector<FRawType> RawTypes;
		FWireReader TypeReader(Sections[1]);
		uint64 TypeCount = 0;
		if (!TypeReader.ReadVarUInt(TypeCount, OutError) || TypeCount > MaximumTableEntries)
			return TypeCount > MaximumTableEntries ? Fail(OutError, "type count exceeds bound") : false;
		RawTypes.reserve(TypeCount);
		for (uint64 Index = 0; Index < TypeCount; ++Index)
		{
			std::span<const std::byte> RecordBytes;
			if (!ReadRecord(TypeReader, RecordBytes, OutError))
				return false;
			FWireReader Record(RecordBytes);
			uint8 Opcode = 0;
			if (!Record.ReadU8(Opcode, OutError) || Opcode < 1 || Opcode > 0x17)
				return Fail(OutError, "unsupported type opcode");
			FRawType Raw{.Opcode = ETypeOpcode(Opcode)};
			switch (Raw.Opcode)
			{
			case ETypeOpcode::Enum:
			{
				uint8 Storage = 0;
				if (!Record.ReadVarUInt(Raw.NameId, OutError) || !Record.ReadU8(Storage, OutError))
					return false;
				Raw.Parameter = Storage;
				break;
			}
			case ETypeOpcode::Intrinsic:
			{
				uint8 Layout = 0;
				if (!Record.ReadU8(Layout, OutError))
					return false;
				Raw.Parameter = Layout;
				break;
			}
			case ETypeOpcode::Struct:
			case ETypeOpcode::HardRef:
			case ETypeOpcode::SoftRef:
				if (!Record.ReadVarUInt(Raw.NameId, OutError))
					return false;
				break;
			case ETypeOpcode::FixedArray:
			{
				uint64 Child = 0;
				if (!Record.ReadVarUInt(Child, OutError)
					|| !Record.ReadVarUInt(Raw.Parameter, OutError))
					return false;
				Raw.Children.push_back(Child);
				break;
			}
			case ETypeOpcode::Array:
			{
				uint64 Child = 0;
				if (!Record.ReadVarUInt(Child, OutError))
					return false;
				Raw.Children.push_back(Child);
				break;
			}
			case ETypeOpcode::Map:
			{
				uint64 Key = 0;
				uint64 Value = 0;
				if (!Record.ReadVarUInt(Key, OutError) || !Record.ReadVarUInt(Value, OutError))
					return false;
				Raw.Children = {Key, Value};
				break;
			}
			default:
				break;
			}
			if (!Record.RequireEnd(OutError))
				return false;
			if ((Raw.Opcode == ETypeOpcode::Struct || Raw.Opcode == ETypeOpcode::Enum)
				&& (Raw.NameId == 0 || Raw.NameId > NameCount))
				return Fail(OutError, "type name id is out of range");
			if ((Raw.Opcode == ETypeOpcode::HardRef || Raw.Opcode == ETypeOpcode::SoftRef)
				&& Raw.NameId > NameCount)
				return Fail(OutError, "reference class name id is out of range");
			for (uint64 Child : Raw.Children)
				if (Child == 0 || Child > TypeCount)
					return Fail(OutError, "child type id is out of range");
			RawTypes.push_back(std::move(Raw));
		}
		if (!TypeReader.RequireEnd(OutError))
			return false;

		Input.Types.reserve(TypeCount);
		for (const FRawType& Raw : RawTypes)
		{
			std::string Name = Raw.NameId == 0 ? std::string{} : Input.AdditionalNames[Raw.NameId - 1];
			Input.Types.push_back(MakeType(Raw.Opcode, std::move(Name), Raw.Parameter));
		}
		for (uint64 Index = 0; Index < TypeCount; ++Index)
			for (uint64 Child : RawTypes[Index].Children)
				Input.Types[Index]->Children.push_back(Input.Types[Child - 1]);

		FWireReader SchemaReader(Sections[2]);
		uint64 CustomCount = 0;
		if (!SchemaReader.ReadVarUInt(CustomCount, OutError) || CustomCount > MaximumCustomVersions)
			return CustomCount > MaximumCustomVersions
				? Fail(OutError, "custom version count exceeds bound") : false;
		for (uint64 Index = 0; Index < CustomCount; ++Index)
		{
			FCustomVersion Custom;
			uint64 Value = 0;
			if (!SchemaReader.ReadU32(Custom.Guid.A, OutError)
				|| !SchemaReader.ReadU32(Custom.Guid.B, OutError)
				|| !SchemaReader.ReadU32(Custom.Guid.C, OutError)
				|| !SchemaReader.ReadU32(Custom.Guid.D, OutError)
				|| !SchemaReader.ReadVarUInt(Value, OutError))
				return false;
			if (Value > std::numeric_limits<uint32>::max())
				return Fail(OutError, "custom version value exceeds uint32");
			Custom.Value = uint32(Value);
			Input.CustomVersions.push_back(Custom);
		}

		uint64 SchemaCount = 0;
		if (!SchemaReader.ReadVarUInt(SchemaCount, OutError) || SchemaCount > MaximumTableEntries)
			return SchemaCount > MaximumTableEntries ? Fail(OutError, "schema count exceeds bound") : false;
		for (uint64 Index = 0; Index < SchemaCount; ++Index)
		{
			std::span<const std::byte> RecordBytes;
			if (!ReadRecord(SchemaReader, RecordBytes, OutError))
				return false;
			FWireReader Record(RecordBytes);
			uint64 NameId = 0;
			uint64 FieldCount = 0;
			if (!Record.ReadVarUInt(NameId, OutError) || NameId == 0 || NameId > NameCount
				|| !Record.ReadVarUInt(FieldCount, OutError) || FieldCount > MaximumSchemaFields)
				return Fail(OutError, "schema header id or field count is invalid");
			FSchemaDescriptor Schema{.QualifiedName = Input.AdditionalNames[NameId - 1]};
			for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
			{
				uint64 FieldNameId = 0;
				uint64 TypeId = 0;
				uint64 Flags = 0;
				if (!Record.ReadVarUInt(FieldNameId, OutError) || FieldNameId == 0 || FieldNameId > NameCount
					|| !Record.ReadVarUInt(TypeId, OutError) || TypeId == 0 || TypeId > TypeCount
					|| !Record.ReadVarUInt(Flags, OutError))
					return Fail(OutError, "schema field id is out of range");
				Schema.Fields.push_back({Input.AdditionalNames[FieldNameId - 1], Input.Types[TypeId - 1], Flags});
			}
			if (!Record.RequireEnd(OutError))
				return false;
			Input.Schemas.push_back(std::move(Schema));
		}
		if (!SchemaReader.RequireEnd(OutError))
			return false;

		FWireReader ObjectReader(Sections[3]);
		uint64 ObjectCount = 0;
		if (!ObjectReader.ReadVarUInt(ObjectCount, OutError) || ObjectCount > MaximumObjects)
			return ObjectCount > MaximumObjects ? Fail(OutError, "object count exceeds bound") : false;
		for (uint64 Index = 0; Index < ObjectCount; ++Index)
		{
			std::span<const std::byte> RecordBytes;
			if (!ReadRecord(ObjectReader, RecordBytes, OutError))
				return false;
			FWireReader Record(RecordBytes);
			uint64 OuterId = 0;
			uint64 ClassId = 0;
			uint64 ObjectNameId = 0;
			if (!Record.ReadVarUInt(OuterId, OutError)
				|| !Record.ReadVarUInt(ClassId, OutError) || ClassId == 0 || ClassId > NameCount
				|| !Record.ReadVarUInt(ObjectNameId, OutError) || ObjectNameId == 0 || ObjectNameId > NameCount
				|| !Record.RequireEnd(OutError))
				return Fail(OutError, "object record is invalid");
			if ((Index == 0 && OuterId != 0) || (Index > 0 && (OuterId == 0 || OuterId > Index)))
				return Fail(OutError, "object outer id is not canonical");
			const std::string OuterPath = OuterId == 0 ? std::string{} : Input.Objects[OuterId - 1].Path;
			const std::string ObjectName = Input.AdditionalNames[ObjectNameId - 1];
			Input.Objects.push_back({
				.Path = OuterPath.empty() ? ObjectName : OuterPath + "/" + ObjectName,
				.OuterPath = OuterPath,
				.ClassName = Input.AdditionalNames[ClassId - 1],
				.ObjectName = ObjectName,
			});
		}
		if (!ObjectReader.RequireEnd(OutError))
			return false;

		FFrozenTables Result;
		if (!FreezeTables(Input, Result, OutError))
			return false;
		std::array<std::vector<std::byte>, 4> Canonical;
		if (!EncodeTableSections(Result, Canonical, OutError))
			return false;
		if (Canonical != Sections)
			return Fail(OutError, "table sections are not canonical");
		OutTables = std::move(Result);
		return true;
	}

	namespace
	{
		auto SignedFits(ETypeOpcode Opcode, int64 Value) -> bool
		{
			switch (Opcode)
			{
			case ETypeOpcode::I8: return Value >= -128 && Value <= 127;
			case ETypeOpcode::I16: return Value >= -32768 && Value <= 32767;
			case ETypeOpcode::I32:
				return Value >= std::numeric_limits<int32>::min()
					&& Value <= std::numeric_limits<int32>::max();
			case ETypeOpcode::I64: return true;
			default: return false;
			}
		}

		auto UnsignedFits(ETypeOpcode Opcode, uint64 Value) -> bool
		{
			switch (Opcode)
			{
			case ETypeOpcode::U8: return Value <= std::numeric_limits<uint8>::max();
			case ETypeOpcode::U16: return Value <= std::numeric_limits<uint16>::max();
			case ETypeOpcode::U32: return Value <= std::numeric_limits<uint32>::max();
			case ETypeOpcode::U64: return true;
			default: return false;
			}
		}

		auto EncodeValueInner(
			const FTypeDescriptor& Type,
			const FValue& Value,
			const FFrozenTables& Tables,
			FWireWriter& Writer,
			uint32 Depth,
			std::string& OutError) -> bool
		{
			if (Depth > MaximumValueDepth)
				return Fail(OutError, "value nesting depth exceeds bound");
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool:
				Writer.WriteU8(Value.Bool ? 1 : 0);
				return true;
			case ETypeOpcode::I8:
			case ETypeOpcode::I16:
			case ETypeOpcode::I32:
			case ETypeOpcode::I64:
				if (!SignedFits(Type.Opcode, Value.Signed))
					return Fail(OutError, "signed integer is out of range");
				Writer.WriteVarInt(Value.Signed);
				return true;
			case ETypeOpcode::U8:
			case ETypeOpcode::U16:
			case ETypeOpcode::U32:
			case ETypeOpcode::U64:
				if (!UnsignedFits(Type.Opcode, Value.Unsigned))
					return Fail(OutError, "unsigned integer is out of range");
				Writer.WriteVarUInt(Value.Unsigned);
				return true;
			case ETypeOpcode::F32:
			{
				const float Number = float(Value.Number);
				Writer.WriteU32(std::isnan(Number) ? 0x7fc00000u : std::bit_cast<uint32>(Number));
				return true;
			}
			case ETypeOpcode::F64:
				Writer.WriteU64(std::isnan(Value.Number)
					? 0x7ff8000000000000ull : std::bit_cast<uint64>(Value.Number));
				return true;
			case ETypeOpcode::String:
				return Writer.WriteString(Value.Text, OutError);
			case ETypeOpcode::Name:
			{
				const uint64 Id = Tables.NameId(Value.Text);
				if (Id == 0)
					return Fail(OutError, "name value was not discovered");
				Writer.WriteVarUInt(Id);
				return true;
			}
			case ETypeOpcode::Guid:
				Writer.WriteU32(Value.Guid.A);
				Writer.WriteU32(Value.Guid.B);
				Writer.WriteU32(Value.Guid.C);
				Writer.WriteU32(Value.Guid.D);
				return true;
			case ETypeOpcode::Enum:
			{
				const ETypeOpcode Storage = ETypeOpcode(Type.Parameter);
				if (Storage >= ETypeOpcode::I8 && Storage <= ETypeOpcode::I64)
				{
					if (!SignedFits(Storage, Value.Signed))
						return Fail(OutError, "signed enum is out of range");
					Writer.WriteVarInt(Value.Signed);
				}
				else if (Storage >= ETypeOpcode::U8 && Storage <= ETypeOpcode::U64)
				{
					if (!UnsignedFits(Storage, Value.Unsigned))
						return Fail(OutError, "unsigned enum is out of range");
					Writer.WriteVarUInt(Value.Unsigned);
				}
				else
					return Fail(OutError, "enum storage opcode is invalid");
				return true;
			}
			case ETypeOpcode::Intrinsic:
			{
				const uint64 ComponentCount = Type.Parameter == 1 ? 2
					: Type.Parameter == 2 ? 3 : Type.Parameter == 5 ? 10 : 4;
				if (Value.Components.size() != ComponentCount)
					return Fail(OutError, "intrinsic component count mismatch");
				for (double Component : Value.Components)
				{
					if (Type.Parameter == 6)
					{
						const float F32 = float(Component);
						Writer.WriteU32(std::isnan(F32) ? 0x7fc00000u : std::bit_cast<uint32>(F32));
					}
					else
						Writer.WriteU64(std::isnan(Component)
							? 0x7ff8000000000000ull : std::bit_cast<uint64>(Component));
				}
				return true;
			}
			case ETypeOpcode::Struct:
			{
				if (Type.HasCustomSerializer)
					return Fail(OutError, "struct custom serializer has no object-stream codec");
				if (!Type.HasDeterministicStructOperations)
					return Fail(OutError, "struct operations are unavailable");
				const uint64 SchemaId = Tables.SchemaId(Type.QualifiedName);
				if (SchemaId == 0)
					return Fail(OutError, "struct schema was not discovered");
				if (Value.Elements.size() != Value.FieldIds.size()
					|| Value.Elements.size() != Value.Provenances.size())
					return Fail(OutError, "struct changed-field vectors mismatch");
				if (Value.Elements.size() > MaximumSchemaFields)
					return Fail(OutError, "struct changed-field count exceeds bound");
				Writer.WriteVarUInt(Value.Elements.size());
				uint64 Previous = 0;
				for (uint64 Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const uint64 FieldId = Value.FieldIds[Index];
					if (FieldId <= Previous || FieldId > Tables.Schemas[SchemaId - 1].Fields.size())
						return Fail(OutError, "struct fields are duplicate, unordered, or out of range");
					if (Value.Provenances[Index] > 1)
						return Fail(OutError, "known struct provenance is invalid");
					FWireWriter EncodedWriter;
					if (!EncodeValueInner(*Tables.Schemas[SchemaId - 1].Fields[FieldId - 1].Type,
						Value.Elements[Index], Tables, EncodedWriter, Depth + 1, OutError))
						return false;
					const std::vector<std::byte> Encoded = EncodedWriter.TakeBytes();
					Writer.WriteVarUInt(FieldId);
					Writer.WriteU8(Value.Provenances[Index]);
					WriteRecord(Writer, Encoded);
					Previous = FieldId;
				}
				return true;
			}
			case ETypeOpcode::FixedArray:
				if (Value.Elements.size() != Type.Parameter)
					return Fail(OutError, "fixed-array dimension mismatch");
				for (const FValue& Element : Value.Elements)
					if (!EncodeValueInner(*Type.Children[0], Element, Tables, Writer, Depth + 1, OutError))
						return false;
				return true;
			case ETypeOpcode::Array:
				if (Value.Elements.size() > MaximumContainerElements)
					return Fail(OutError, "array count exceeds bound");
				Writer.WriteVarUInt(Value.Elements.size());
				for (const FValue& Element : Value.Elements)
					if (!EncodeValueInner(*Type.Children[0], Element, Tables, Writer, Depth + 1, OutError))
						return false;
				return true;
			case ETypeOpcode::Map:
			{
				if (Value.Elements.size() % 2 != 0 || Value.Elements.size() / 2 > MaximumContainerElements)
					return Fail(OutError, "map entry count is invalid");
				struct FEntry { std::vector<std::byte> Key; std::vector<std::byte> Value; };
				std::vector<FEntry> Entries;
				for (uint64 Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FEntry Entry;
					FWireWriter KeyWriter;
					FWireWriter ValueWriter;
					if (!EncodeValueInner(*Type.Children[0], Value.Elements[Index], Tables,
						KeyWriter, Depth + 1, OutError)
						|| !EncodeValueInner(*Type.Children[1], Value.Elements[Index + 1], Tables,
							ValueWriter, Depth + 1, OutError))
						return false;
					Entry.Key = KeyWriter.TakeBytes();
					Entry.Value = ValueWriter.TakeBytes();
					Entries.push_back(std::move(Entry));
				}
				std::sort(Entries.begin(), Entries.end(),
					[](const FEntry& Left, const FEntry& Right) { return ByteLess(Left.Key, Right.Key); });
				for (uint64 Index = 1; Index < Entries.size(); ++Index)
					if (Entries[Index - 1].Key == Entries[Index].Key)
						return Fail(OutError, "duplicate logical map key");
				Writer.WriteVarUInt(Entries.size());
				for (const FEntry& Entry : Entries)
				{
					Writer.WriteBytes(Entry.Key);
					Writer.WriteBytes(Entry.Value);
				}
				return true;
			}
			case ETypeOpcode::HardRef:
				if (Value.ReferenceTag > 2)
					return Fail(OutError, "hard reference tag is invalid");
				Writer.WriteU8(Value.ReferenceTag);
				if (Value.ReferenceTag != 0)
				{
					if (Value.ReferenceId == 0 || (Value.ReferenceTag == 1
						&& Value.ReferenceId > Tables.Objects.size())
						|| (Value.ReferenceTag == 2
							&& Value.ReferenceId > Tables.PublicDependencyCount))
						return Fail(OutError, "hard reference id is out of range");
					Writer.WriteVarUInt(Value.ReferenceId);
				}
				return true;
			case ETypeOpcode::SoftRef:
				if (Value.ReferenceTag > 1)
					return Fail(OutError, "soft reference tag is invalid");
				Writer.WriteU8(Value.ReferenceTag);
				if (Value.ReferenceTag == 1)
				{
					const uint64 NameId = Tables.NameId(Value.Text);
					if (NameId == 0)
						return Fail(OutError, "soft reference path was not discovered");
					Writer.WriteVarUInt(NameId);
				}
				return true;
			case ETypeOpcode::Bytes:
				if (Value.ByteData.size() > MaximumContainerElements)
					return Fail(OutError, "byte value exceeds bound");
				Writer.WriteVarUInt(Value.ByteData.size());
				Writer.WriteBytes(Value.ByteData);
				return true;
			}
			return Fail(OutError, "unsupported value opcode");
		}

		auto DecodeValueInner(
			const FTypeDescriptor& Type,
			FWireReader& Reader,
			const FFrozenTables& Tables,
			FValue& OutValue,
			uint32 Depth,
			std::string& OutError) -> bool
		{
			if (Depth > MaximumValueDepth)
				return Fail(OutError, "value nesting depth exceeds bound");
			FValue Value;
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool:
			{
				uint8 Bool = 0;
				if (!Reader.ReadU8(Bool, OutError) || Bool > 1)
					return Fail(OutError, "bool value is invalid");
				Value.Bool = Bool != 0;
				break;
			}
			case ETypeOpcode::I8:
			case ETypeOpcode::I16:
			case ETypeOpcode::I32:
			case ETypeOpcode::I64:
				if (!Reader.ReadVarInt(Value.Signed, OutError) || !SignedFits(Type.Opcode, Value.Signed))
					return Fail(OutError, "signed integer is out of range");
				break;
			case ETypeOpcode::U8:
			case ETypeOpcode::U16:
			case ETypeOpcode::U32:
			case ETypeOpcode::U64:
				if (!Reader.ReadVarUInt(Value.Unsigned, OutError) || !UnsignedFits(Type.Opcode, Value.Unsigned))
					return Fail(OutError, "unsigned integer is out of range");
				break;
			case ETypeOpcode::F32:
			{
				uint32 Bits = 0;
				if (!Reader.ReadU32(Bits, OutError))
					return false;
				if (std::isnan(std::bit_cast<float>(Bits)) && Bits != 0x7fc00000u)
					return Fail(OutError, "noncanonical F32 NaN");
				Value.Number = std::bit_cast<float>(Bits);
				break;
			}
			case ETypeOpcode::F64:
			{
				uint64 Bits = 0;
				if (!Reader.ReadU64(Bits, OutError))
					return false;
				if (std::isnan(std::bit_cast<double>(Bits)) && Bits != 0x7ff8000000000000ull)
					return Fail(OutError, "noncanonical F64 NaN");
				Value.Number = std::bit_cast<double>(Bits);
				break;
			}
			case ETypeOpcode::String:
				if (!Reader.ReadString(Value.Text, OutError))
					return false;
				break;
			case ETypeOpcode::Name:
			{
				uint64 Id = 0;
				if (!Reader.ReadVarUInt(Id, OutError) || Id == 0 || Id > Tables.Names.size())
					return Fail(OutError, "name value id is out of range");
				Value.Text = Tables.Names[Id - 1];
				break;
			}
			case ETypeOpcode::Guid:
				if (!Reader.ReadU32(Value.Guid.A, OutError) || !Reader.ReadU32(Value.Guid.B, OutError)
					|| !Reader.ReadU32(Value.Guid.C, OutError) || !Reader.ReadU32(Value.Guid.D, OutError))
					return false;
				break;
			case ETypeOpcode::Enum:
			{
				const ETypeOpcode Storage = ETypeOpcode(Type.Parameter);
				if (Storage >= ETypeOpcode::I8 && Storage <= ETypeOpcode::I64)
				{
					if (!Reader.ReadVarInt(Value.Signed, OutError) || !SignedFits(Storage, Value.Signed))
						return Fail(OutError, "signed enum is out of range");
				}
				else if (Storage >= ETypeOpcode::U8 && Storage <= ETypeOpcode::U64)
				{
					if (!Reader.ReadVarUInt(Value.Unsigned, OutError) || !UnsignedFits(Storage, Value.Unsigned))
						return Fail(OutError, "unsigned enum is out of range");
				}
				else
					return Fail(OutError, "enum storage opcode is invalid");
				break;
			}
			case ETypeOpcode::Intrinsic:
			{
				const uint64 Count = Type.Parameter == 1 ? 2
					: Type.Parameter == 2 ? 3 : Type.Parameter == 5 ? 10 : 4;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					if (Type.Parameter == 6)
					{
						uint32 Bits = 0;
						if (!Reader.ReadU32(Bits, OutError)) return false;
						if (std::isnan(std::bit_cast<float>(Bits)) && Bits != 0x7fc00000u)
							return Fail(OutError, "noncanonical intrinsic F32 NaN");
						Value.Components.push_back(std::bit_cast<float>(Bits));
					}
					else
					{
						uint64 Bits = 0;
						if (!Reader.ReadU64(Bits, OutError)) return false;
						if (std::isnan(std::bit_cast<double>(Bits)) && Bits != 0x7ff8000000000000ull)
							return Fail(OutError, "noncanonical intrinsic F64 NaN");
						Value.Components.push_back(std::bit_cast<double>(Bits));
					}
				}
				break;
			}
			case ETypeOpcode::Struct:
			{
				if (Type.HasCustomSerializer)
					return Fail(OutError, "struct custom serializer has no object-stream codec");
				if (!Type.HasDeterministicStructOperations)
					return Fail(OutError, "struct operations are unavailable");
				const uint64 SchemaId = Tables.SchemaId(Type.QualifiedName);
				if (SchemaId == 0)
					return Fail(OutError, "struct schema was not discovered");
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count, OutError) || Count > MaximumSchemaFields)
					return Count > MaximumSchemaFields
						? Fail(OutError, "struct changed-field count exceeds bound") : false;
				uint64 Previous = 0;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					uint64 FieldId = 0;
					uint8 Provenance = 0;
					std::span<const std::byte> Encoded;
					if (!Reader.ReadVarUInt(FieldId, OutError) || FieldId <= Previous
						|| FieldId > Tables.Schemas[SchemaId - 1].Fields.size()
						|| !Reader.ReadU8(Provenance, OutError) || Provenance > 1
						|| !ReadRecord(Reader, Encoded, OutError))
						return Fail(OutError, "struct field record is invalid or noncanonical provenance");
					FWireReader FieldReader(Encoded);
					FValue FieldValue;
					if (!DecodeValueInner(*Tables.Schemas[SchemaId - 1].Fields[FieldId - 1].Type,
						FieldReader, Tables, FieldValue, Depth + 1, OutError)
						|| !FieldReader.RequireEnd(OutError))
						return false;
					Value.FieldIds.push_back(FieldId);
					Value.Provenances.push_back(Provenance);
					Value.Elements.push_back(std::move(FieldValue));
					Previous = FieldId;
				}
				break;
			}
			case ETypeOpcode::FixedArray:
				for (uint64 Index = 0; Index < Type.Parameter; ++Index)
				{
					FValue Element;
					if (!DecodeValueInner(*Type.Children[0], Reader, Tables, Element, Depth + 1, OutError))
						return false;
					Value.Elements.push_back(std::move(Element));
				}
				break;
			case ETypeOpcode::Array:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count, OutError) || Count > MaximumContainerElements)
					return Count > MaximumContainerElements ? Fail(OutError, "array count exceeds bound") : false;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FValue Element;
					if (!DecodeValueInner(*Type.Children[0], Reader, Tables, Element, Depth + 1, OutError))
						return false;
					Value.Elements.push_back(std::move(Element));
				}
				break;
			}
			case ETypeOpcode::Map:
			{
				uint64 Count = 0;
				if (!Reader.ReadVarUInt(Count, OutError) || Count > MaximumContainerElements)
					return Count > MaximumContainerElements ? Fail(OutError, "map count exceeds bound") : false;
				std::vector<std::byte> PreviousKey;
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FValue Key;
					FValue MapValue;
					if (!DecodeValueInner(*Type.Children[0], Reader, Tables, Key, Depth + 1, OutError))
						return false;
					std::vector<std::byte> EncodedKey;
					if (!EncodeValue(*Type.Children[0], Key, Tables, EncodedKey, OutError))
						return false;
					if (!PreviousKey.empty() && !ByteLess(PreviousKey, EncodedKey))
						return Fail(OutError, "map keys are duplicate or noncanonical");
					if (!DecodeValueInner(*Type.Children[1], Reader, Tables, MapValue, Depth + 1, OutError))
						return false;
					Value.Elements.push_back(std::move(Key));
					Value.Elements.push_back(std::move(MapValue));
					PreviousKey = std::move(EncodedKey);
				}
				break;
			}
			case ETypeOpcode::HardRef:
				if (!Reader.ReadU8(Value.ReferenceTag, OutError) || Value.ReferenceTag > 2)
					return Fail(OutError, "hard reference tag is invalid");
				if (Value.ReferenceTag != 0
					&& (!Reader.ReadVarUInt(Value.ReferenceId, OutError) || Value.ReferenceId == 0
						|| (Value.ReferenceTag == 1 && Value.ReferenceId > Tables.Objects.size())
						|| (Value.ReferenceTag == 2
							&& Value.ReferenceId > Tables.PublicDependencyCount)))
					return Fail(OutError, "hard reference id is out of range");
				break;
			case ETypeOpcode::SoftRef:
				if (!Reader.ReadU8(Value.ReferenceTag, OutError) || Value.ReferenceTag > 1)
					return Fail(OutError, "soft reference tag is invalid");
				if (Value.ReferenceTag == 1)
				{
					if (!Reader.ReadVarUInt(Value.ReferenceId, OutError) || Value.ReferenceId == 0
						|| Value.ReferenceId > Tables.Names.size())
						return Fail(OutError, "soft reference name id is out of range");
					Value.Text = Tables.Names[Value.ReferenceId - 1];
				}
				break;
			case ETypeOpcode::Bytes:
			{
				uint64 Count = 0;
				std::span<const std::byte> Data;
				if (!Reader.ReadVarUInt(Count, OutError) || Count > MaximumContainerElements
					|| !Reader.ReadBytes(Count, Data, OutError))
					return Fail(OutError, "byte value is invalid or exceeds bound");
				Value.ByteData.assign(Data.begin(), Data.end());
				break;
			}
			}
			OutValue = std::move(Value);
			return true;
		}
	}

	auto EncodeValue(
		const FTypeDescriptor& Type,
		const FValue& Value,
		const FFrozenTables& Tables,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		std::vector<std::byte> Key;
		if (!StructuralKey(Type, Key, OutError))
			return false;
		FWireWriter Writer;
		if (!EncodeValueInner(Type, Value, Tables, Writer, 0, OutError))
			return false;
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto DecodeValue(
		const FTypeDescriptor& Type,
		std::span<const std::byte> Bytes,
		const FFrozenTables& Tables,
		FValue& OutValue,
		std::string& OutError) -> bool
	{
		std::vector<std::byte> Key;
		if (!StructuralKey(Type, Key, OutError))
			return false;
		FWireReader Reader(Bytes);
		FValue Result;
		if (!DecodeValueInner(Type, Reader, Tables, Result, 0, OutError)
			|| !Reader.RequireEnd(OutError))
			return false;
		OutValue = std::move(Result);
		return true;
	}

	auto EncodeOverrideBlock(
		std::span<const FOverrideCandidate> Candidates,
		const FFrozenTables& Tables,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		struct FOverride
		{
			uint64 SchemaId = 0;
			uint64 FieldId = 0;
			uint8 Provenance = 0;
			std::vector<std::byte> Bytes;
		};
		std::vector<FOverride> Overrides;
		for (const FOverrideCandidate& Candidate : Candidates)
		{
			const uint64 SchemaId = Tables.SchemaId(Candidate.SchemaName);
			const uint64 FieldId = Tables.FieldId(SchemaId, Candidate.FieldName);
			const FTypeDescriptor* Type = nullptr;
			if (!FindFieldType(Tables, SchemaId, FieldId, Type, OutError))
				return false;
			if (Type->Opcode == ETypeOpcode::Struct)
			{
				if (Type->HasCustomSerializer)
					return Fail(OutError, "struct custom serializer has no object-stream codec");
				if (!Type->HasDeterministicStructOperations)
					return Fail(OutError, "struct operations are unavailable");
			}
			if (!Candidate.LoadedExplicit && !Candidate.Forced
				&& Candidate.Value == Candidate.DefaultValue)
				continue;
			FOverride Override{
				.SchemaId = SchemaId,
				.FieldId = FieldId,
				.Provenance = Candidate.Forced ? uint8(1) : uint8(0),
			};
			if (!EncodeValue(*Type, Candidate.Value, Tables, Override.Bytes, OutError))
				return false;
			Overrides.push_back(std::move(Override));
		}
		std::sort(Overrides.begin(), Overrides.end(),
			[](const FOverride& Left, const FOverride& Right)
			{
				return Left.SchemaId != Right.SchemaId
					? Left.SchemaId < Right.SchemaId : Left.FieldId < Right.FieldId;
			});
		for (uint64 Index = 1; Index < Overrides.size(); ++Index)
			if (Overrides[Index - 1].SchemaId == Overrides[Index].SchemaId
				&& Overrides[Index - 1].FieldId == Overrides[Index].FieldId)
				return Fail(OutError, "duplicate object override");

		FWireWriter Writer;
		Writer.WriteVarUInt(Overrides.size());
		for (const FOverride& Override : Overrides)
		{
			Writer.WriteVarUInt(Override.SchemaId);
			Writer.WriteVarUInt(Override.FieldId);
			Writer.WriteU8(Override.Provenance);
			WriteRecord(Writer, Override.Bytes);
		}
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto EncodeValueSection(
		std::span<const FObjectValueInput> Objects,
		const FFrozenTables& Tables,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		if (Objects.size() != Tables.Objects.size())
			return Fail(OutError, "value section object count mismatch");
		std::vector<const FObjectValueInput*> ById(Tables.Objects.size(), nullptr);
		for (const FObjectValueInput& Object : Objects)
		{
			const uint64 ObjectId = Tables.ObjectId(Object.ObjectPath);
			if (ObjectId == 0)
				return Fail(OutError, "value section object id is out of range");
			if (ById[ObjectId - 1])
				return Fail(OutError, "duplicate value section object");
			ById[ObjectId - 1] = &Object;
		}
		FWireWriter Writer;
		Writer.WriteVarUInt(ById.size());
		for (const FObjectValueInput* Object : ById)
		{
			if (!Object)
				return Fail(OutError, "value section object is missing");
			std::vector<std::byte> Block;
			if (!EncodeOverrideBlock(Object->Overrides, Tables, Block, OutError))
				return false;
			WriteRecord(Writer, Block);
		}
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto ValidateValueSection(
		std::span<const std::byte> Bytes,
		const FFrozenTables& Tables,
		std::string& OutError) -> bool
	{
		FWireReader Reader(Bytes);
		uint64 ObjectCount = 0;
		if (!Reader.ReadVarUInt(ObjectCount, OutError) || ObjectCount != Tables.Objects.size())
			return Fail(OutError, "value section object count mismatch");
		for (uint64 ObjectIndex = 0; ObjectIndex < ObjectCount; ++ObjectIndex)
		{
			std::span<const std::byte> BlockBytes;
			if (!ReadRecord(Reader, BlockBytes, OutError))
				return false;
			FWireReader Block(BlockBytes);
			uint64 OverrideCount = 0;
			if (!Block.ReadVarUInt(OverrideCount, OutError) || OverrideCount > MaximumTableEntries)
				return Fail(OutError, "override count is invalid");
			uint64 PreviousSchema = 0;
			uint64 PreviousField = 0;
			for (uint64 Index = 0; Index < OverrideCount; ++Index)
			{
				uint64 SchemaId = 0;
				uint64 FieldId = 0;
				uint8 Provenance = 0;
				std::span<const std::byte> ValueBytes;
				if (!Block.ReadVarUInt(SchemaId, OutError)
					|| !Block.ReadVarUInt(FieldId, OutError)
					|| (SchemaId < PreviousSchema
						|| (SchemaId == PreviousSchema && FieldId <= PreviousField))
					|| !Block.ReadU8(Provenance, OutError) || Provenance > 2
					|| !ReadRecord(Block, ValueBytes, OutError))
					return Fail(OutError, "override record is unordered, duplicate, or invalid");
				const FTypeDescriptor* Type = nullptr;
				if (!FindFieldType(Tables, SchemaId, FieldId, Type, OutError))
					return false;
				if (Provenance == 2)
				{
					std::vector<std::byte> Closure;
					std::vector<std::byte> Payload;
					if (!ValidateUnknownValueBody(ValueBytes, Closure, Payload, OutError))
						return false;
				}
				else
				{
					FValue Value;
					if (!DecodeValue(*Type, ValueBytes, Tables, Value, OutError))
						return false;
				}
				PreviousSchema = SchemaId;
				PreviousField = FieldId;
			}
			if (!Block.RequireEnd(OutError))
				return false;
		}
		return Reader.RequireEnd(OutError);
	}

	auto EncodeRetainedClosure(
		const FFrozenTables& Tables,
		uint64 RootSchemaId,
		uint64 RootFieldId,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!Tables.CustomVersions.empty() || !Tables.Objects.empty())
			return Fail(OutError, "retained closure may contain only Name/Type/Schema tables");
		const FTypeDescriptor* RootType = nullptr;
		if (!FindFieldType(Tables, RootSchemaId, RootFieldId, RootType, OutError))
			return false;
		std::array<std::vector<std::byte>, 4> Sections;
		if (!EncodeTableSections(Tables, Sections, OutError))
			return false;
		FWireWriter Writer;
		for (uint64 Index = 0; Index < 3; ++Index)
			WriteRecord(Writer, Sections[Index]);
		Writer.WriteVarUInt(RootSchemaId);
		Writer.WriteVarUInt(RootFieldId);
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto ValidateRetainedClosure(
		std::span<const std::byte> Bytes,
		std::string& OutError) -> bool
	{
		FWireReader Reader(Bytes);
		std::array<std::vector<std::byte>, 4> Sections;
		for (uint64 Index = 0; Index < 3; ++Index)
		{
			std::span<const std::byte> Section;
			if (!ReadRecord(Reader, Section, OutError))
				return false;
			Sections[Index].assign(Section.begin(), Section.end());
		}
		Sections[3] = {std::byte{0}};
		uint64 RootSchemaId = 0;
		uint64 RootFieldId = 0;
		if (!Reader.ReadVarUInt(RootSchemaId, OutError)
			|| !Reader.ReadVarUInt(RootFieldId, OutError)
			|| !Reader.RequireEnd(OutError))
			return false;
		FFrozenTables Tables;
		if (!DecodeTableSections(Sections, Tables, OutError))
			return false;
		const FTypeDescriptor* RootType = nullptr;
		return FindFieldType(Tables, RootSchemaId, RootFieldId, RootType, OutError);
	}

	auto EncodeUnknownValueBody(
		std::span<const std::byte> Closure,
		std::span<const std::byte> Payload,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!ValidateRetainedClosure(Closure, OutError))
			return false;
		FWireWriter Writer;
		WriteRecord(Writer, Closure);
		WriteRecord(Writer, Payload);
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto ValidateUnknownValueBody(
		std::span<const std::byte> Bytes,
		std::vector<std::byte>& OutClosure,
		std::vector<std::byte>& OutPayload,
		std::string& OutError) -> bool
	{
		FWireReader Reader(Bytes);
		std::span<const std::byte> Closure;
		std::span<const std::byte> Payload;
		if (!ReadRecord(Reader, Closure, OutError) || !ValidateRetainedClosure(Closure, OutError)
			|| !ReadRecord(Reader, Payload, OutError) || !Reader.RequireEnd(OutError))
			return false;
		std::vector<std::byte> ClosureCopy(Closure.begin(), Closure.end());
		std::vector<std::byte> PayloadCopy(Payload.begin(), Payload.end());
		OutClosure = std::move(ClosureCopy);
		OutPayload = std::move(PayloadCopy);
		return true;
	}
}
