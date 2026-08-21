#include "Asset/PackageV4Writer.h"

namespace Durin::Asset::DastV4
{
	namespace
	{
		constexpr uint32 MaximumSummaryBytes = 65'535;
		constexpr uint8 SectionCount = 5;

		auto Fail(FWriterDiagnostic& Diagnostic, EWriterFailure Failure,
			std::string Message, std::string LogicalPath = {}) -> bool
		{
			Diagnostic = {Failure, std::move(LogicalPath), std::move(Message)};
			return false;
		}

		auto ByteLess(std::string_view Left, std::string_view Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end(),
				[](char A, char B) { return uint8(A) < uint8(B); });
		}

		auto ByteLess(std::span<const uint8> Left, std::span<const uint8> Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end());
		}

		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 First = uint8(Value[Index]);
				if (First == 0) return false;
				if (First < 0x80) { ++Index; continue; }
				uint32 CodePoint = 0;
				size_t Count = 0;
				if (First >= 0xc2 && First <= 0xdf) { CodePoint = First & 0x1f; Count = 2; }
				else if (First >= 0xe0 && First <= 0xef) { CodePoint = First & 0x0f; Count = 3; }
				else if (First >= 0xf0 && First <= 0xf4) { CodePoint = First & 0x07; Count = 4; }
				else return false;
				if (Count > Value.size() - Index) return false;
				for (size_t Part = 1; Part < Count; ++Part)
				{
					const uint8 Continuation = uint8(Value[Index + Part]);
					if ((Continuation & 0xc0) != 0x80) return false;
					CodePoint = (CodePoint << 6) | (Continuation & 0x3f);
				}
				if ((Count == 2 && CodePoint < 0x80) || (Count == 3 && CodePoint < 0x800)
					|| (Count == 4 && CodePoint < 0x10000) || CodePoint > 0x10ffff
					|| (CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
				Index += Count;
			}
			return true;
		}

		class FWireWriter
		{
		public:
			auto U8(uint8 Value) -> void { Data.push_back(Value); }
			auto U16(uint16 Value) -> void { Fixed(Value); }
			auto U32(uint32 Value) -> void { Fixed(Value); }
			auto U64(uint64 Value) -> void { Fixed(Value); }
			auto VarUInt(uint64 Value) -> void
			{
				do
				{
					uint8 Byte = uint8(Value & 0x7f);
					Value >>= 7;
					if (Value != 0) Byte |= 0x80;
					Data.push_back(Byte);
				} while (Value != 0);
			}
			auto VarInt(int64 Value) -> void
			{
				VarUInt((uint64(Value) << 1) ^ uint64(Value >> 63));
			}
			auto String(std::string_view Value) -> void
			{
				VarUInt(Value.size());
				Data.insert(Data.end(), Value.begin(), Value.end());
			}
			auto Bytes(std::span<const uint8> Value) -> void
			{
				Data.insert(Data.end(), Value.begin(), Value.end());
			}
			auto Record(std::span<const uint8> Value) -> void { VarUInt(Value.size()); Bytes(Value); }
			auto View() const -> std::span<const uint8> { return Data; }
			auto Take() -> std::vector<uint8> { return std::move(Data); }
		private:
			template<typename T> auto Fixed(T Value) -> void
			{
				for (size_t Index = 0; Index < sizeof(T); ++Index)
					Data.push_back(uint8(uint64(Value) >> (Index * 8)));
			}
			std::vector<uint8> Data;
		};

		class FWireReader
		{
		public:
			explicit FWireReader(std::span<const uint8> In) : Data(In) {}
			auto U8(uint8& Out) -> bool
			{
				if (Offset == Data.size()) return false;
				Out = Data[Offset++]; return true;
			}
			auto VarUInt(uint64& Out) -> bool
			{
				Out = 0;
				for (uint32 Index = 0; Index < 10; ++Index)
				{
					uint8 Byte = 0;
					if (!U8(Byte) || (Index == 9 && (Byte & 0xfe) != 0)) return false;
					Out |= uint64(Byte & 0x7f) << (Index * 7);
					if ((Byte & 0x80) == 0) return Index == 0 || (Byte & 0x7f) != 0;
				}
				return false;
			}
			auto Bytes(uint64 Count, std::span<const uint8>& Out) -> bool
			{
				if (Count > Data.size() - Offset) return false;
				Out = Data.subspan(Offset, size_t(Count)); Offset += size_t(Count); return true;
			}
			auto Record(std::span<const uint8>& Out) -> bool
			{
				uint64 Size = 0; return VarUInt(Size) && Bytes(Size, Out);
			}
			auto String(std::string& Out) -> bool
			{
				uint64 Size = 0; std::span<const uint8> Encoded;
				if (!VarUInt(Size) || Size > MaximumStringBytes || !Bytes(Size, Encoded)) return false;
				Out.assign(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
				return IsValidUtf8(Out);
			}
			auto End() const -> bool { return Offset == Data.size(); }
		private:
			std::span<const uint8> Data;
			size_t Offset = 0;
		};

		struct FFrozenType { FTypePtr Descriptor; std::vector<uint8> Key; };
		struct FFrozenTables
		{
			std::vector<std::string> Names;
			std::vector<FFrozenType> Types;
			std::vector<FSchemaDescriptor> Schemas;
			std::vector<FCustomVersion> CustomVersions;
			std::vector<FObjectDescriptor> Objects;
			uint64 DependencyCount = 0;

			auto NameId(std::string_view Name) const -> uint64
			{
				const auto It = std::ranges::find(Names, Name);
				return It == Names.end() ? 0 : uint64(std::distance(Names.begin(), It) + 1);
			}
			auto TypeId(const FTypeDescriptor& Type) const -> uint64;
			auto SchemaId(std::string_view Name) const -> uint64
			{
				const auto It = std::ranges::find(Schemas, Name, &FSchemaDescriptor::QualifiedName);
				return It == Schemas.end() ? 0 : uint64(std::distance(Schemas.begin(), It) + 1);
			}
			auto FieldId(uint64 SchemaId, std::string_view Name) const -> uint64
			{
				if (SchemaId == 0 || SchemaId > Schemas.size()) return 0;
				const auto& Fields = Schemas[SchemaId - 1].Fields;
				const auto It = std::ranges::find(Fields, Name, &FFieldDescriptor::Name);
				return It == Fields.end() ? 0 : uint64(std::distance(Fields.begin(), It) + 1);
			}
			auto ObjectId(std::string_view Path) const -> uint64
			{
				const auto It = std::ranges::find(Objects, Path, &FObjectDescriptor::Path);
				return It == Objects.end() ? 0 : uint64(std::distance(Objects.begin(), It) + 1);
			}
		};

		auto ValidateName(std::string_view Name, FWriterDiagnostic& Diagnostic,
			std::string_view Path = {}) -> bool
		{
			if (Name.empty()) return Fail(Diagnostic, EWriterFailure::InvalidInput,
				"A required name is empty.", std::string(Path));
			if (Name.size() > MaximumStringBytes)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded,
					"A name exceeds the DAST v4 string bound.", std::string(Path));
			if (!IsValidUtf8(Name)) return Fail(Diagnostic, EWriterFailure::InvalidUtf8,
				"A name is not shortest-form UTF-8 or contains NUL.", std::string(Path));
			return true;
		}

		auto IsInteger(ETypeOpcode Opcode) -> bool
		{
			return Opcode >= ETypeOpcode::I8 && Opcode <= ETypeOpcode::U64;
		}

		auto AppendStructuralKey(const FTypeDescriptor& Type, FWireWriter& Writer,
			std::unordered_set<const FTypeDescriptor*>& Visiting,
			FWriterDiagnostic& Diagnostic) -> bool
		{
			if (!Visiting.insert(&Type).second)
				return Fail(Diagnostic, EWriterFailure::DescriptorCycle, "A type descriptor contains a cycle.");
			const uint8 Opcode = uint8(Type.Opcode);
			if (Opcode < uint8(ETypeOpcode::Bool) || Opcode > uint8(ETypeOpcode::Bytes))
				return Fail(Diagnostic, EWriterFailure::UnsupportedType, "A type opcode is unsupported.");
			Writer.U8(Opcode);
			auto Plain = [&]() { return Type.QualifiedName.empty() && Type.Parameter == 0 && Type.Children.empty(); };
			bool Valid = true;
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool: case ETypeOpcode::I8: case ETypeOpcode::I16:
			case ETypeOpcode::I32: case ETypeOpcode::I64: case ETypeOpcode::U8:
			case ETypeOpcode::U16: case ETypeOpcode::U32: case ETypeOpcode::U64:
			case ETypeOpcode::F32: case ETypeOpcode::F64: case ETypeOpcode::String:
			case ETypeOpcode::Name: case ETypeOpcode::Guid: case ETypeOpcode::Bytes:
				Valid = Plain(); break;
			case ETypeOpcode::Enum:
				Valid = Type.Children.empty() && Type.Parameter >= uint8(ETypeOpcode::I8)
					&& Type.Parameter <= uint8(ETypeOpcode::U64)
					&& ValidateName(Type.QualifiedName, Diagnostic);
				if (Valid) { Writer.String(Type.QualifiedName); Writer.U8(uint8(Type.Parameter)); }
				break;
			case ETypeOpcode::Intrinsic:
				Valid = Type.Children.empty() && Type.QualifiedName.empty()
					&& Type.Parameter >= 1 && Type.Parameter <= 6;
				if (Valid) Writer.U8(uint8(Type.Parameter));
				break;
			case ETypeOpcode::Struct:
				Valid = Type.Children.empty() && Type.Parameter == 0
					&& ValidateName(Type.QualifiedName, Diagnostic);
				if (Valid) Writer.String(Type.QualifiedName);
				break;
			case ETypeOpcode::FixedArray:
				Valid = Type.Children.size() == 1 && Type.Children[0] && Type.QualifiedName.empty()
					&& Type.Parameter > 0 && Type.Parameter <= MaximumContainerElements;
				if (Valid) Writer.VarUInt(Type.Parameter);
				break;
			case ETypeOpcode::Array:
				Valid = Type.Children.size() == 1 && Type.Children[0]
					&& Type.QualifiedName.empty() && Type.Parameter == 0; break;
			case ETypeOpcode::Map:
				Valid = Type.Children.size() == 2 && Type.Children[0] && Type.Children[1]
					&& Type.QualifiedName.empty() && Type.Parameter == 0
					&& (Type.Children[0]->Opcode == ETypeOpcode::Bool
						|| IsInteger(Type.Children[0]->Opcode)
						|| Type.Children[0]->Opcode == ETypeOpcode::String
						|| Type.Children[0]->Opcode == ETypeOpcode::Name
						|| Type.Children[0]->Opcode == ETypeOpcode::Guid
						|| Type.Children[0]->Opcode == ETypeOpcode::Enum
						|| Type.Children[0]->Opcode == ETypeOpcode::Intrinsic); break;
			case ETypeOpcode::HardRef: case ETypeOpcode::SoftRef:
				Valid = Type.Children.empty() && Type.Parameter == 0
					&& (Type.QualifiedName.empty() || ValidateName(Type.QualifiedName, Diagnostic));
				if (Valid) Writer.String(Type.QualifiedName);
				break;
			}
			if (!Valid)
				return Diagnostic.Failure != EWriterFailure::None ? false
					: Fail(Diagnostic, EWriterFailure::UnsupportedType, "A type descriptor has an invalid shape.");
			for (const FTypePtr& Child : Type.Children)
				if (!AppendStructuralKey(*Child, Writer, Visiting, Diagnostic)) return false;
			Visiting.erase(&Type);
			return true;
		}

		auto StructuralKey(const FTypeDescriptor& Type, std::vector<uint8>& Out,
			FWriterDiagnostic& Diagnostic) -> bool
		{
			FWireWriter Writer;
			std::unordered_set<const FTypeDescriptor*> Visiting;
			if (!AppendStructuralKey(Type, Writer, Visiting, Diagnostic)) return false;
			Out = Writer.Take(); return true;
		}

		auto FFrozenTables::TypeId(const FTypeDescriptor& Type) const -> uint64
		{
			FWriterDiagnostic Ignored;
			std::vector<uint8> Key;
			if (!StructuralKey(Type, Key, Ignored)) return 0;
			const auto It = std::ranges::find(Types, Key, &FFrozenType::Key);
			return It == Types.end() ? 0 : uint64(std::distance(Types.begin(), It) + 1);
		}

		auto FreezeTables(const FPackageInput& Input, FFrozenTables& Out,
			FWriterDiagnostic& Diagnostic) -> bool
		{
			FFrozenTables Result;
			Result.DependencyCount = Input.Dependencies.size();
			if (Result.DependencyCount > MaximumDependencies)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Dependency count exceeds the DAST v4 bound.");
			std::vector<std::string> Names = Input.AdditionalNames;
			auto AddName = [&](std::string_view Name, std::string_view Path = {}) -> bool {
				if (!ValidateName(Name, Diagnostic, Path)) return false;
				Names.emplace_back(Name); return true;
			};

			std::vector<FTypePtr> DiscoveredTypes;
			std::unordered_set<const FTypeDescriptor*> Walked;
			std::function<bool(const FTypePtr&)> DiscoverType = [&](const FTypePtr& Type) -> bool {
				if (!Type) return Fail(Diagnostic, EWriterFailure::InvalidInput, "A type descriptor is null.");
				std::vector<uint8> Key;
				if (!StructuralKey(*Type, Key, Diagnostic)) return false;
				if (Walked.insert(Type.get()).second)
				{
					DiscoveredTypes.push_back(Type);
					if (!Type->QualifiedName.empty() && !AddName(Type->QualifiedName)) return false;
					for (const auto& Child : Type->Children) if (!DiscoverType(Child)) return false;
				}
				return true;
			};
			for (const auto& Type : Input.Types) if (!DiscoverType(Type)) return false;

			Result.Schemas = Input.Schemas;
			if (Result.Schemas.size() > MaximumTableEntries)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Schema count exceeds the DAST v4 bound.");
			for (auto& Schema : Result.Schemas)
			{
				if (!AddName(Schema.QualifiedName, Schema.QualifiedName)) return false;
				if (Schema.Fields.size() > MaximumSchemaFields)
					return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Schema field count exceeds the DAST v4 bound.", Schema.QualifiedName);
				for (auto& Field : Schema.Fields)
				{
					if (!AddName(Field.Name, Schema.QualifiedName + "::" + Field.Name) || !DiscoverType(Field.Type)) return false;
					if (Field.AuthoredFlags != 0)
						return Fail(Diagnostic, EWriterFailure::UnsupportedType, "DAST v4 supports no authored field flags.", Schema.QualifiedName + "::" + Field.Name);
				}
				std::ranges::sort(Schema.Fields, [&](const auto& A, const auto& B) {
					if (A.Name != B.Name) return ByteLess(A.Name, B.Name);
					FWriterDiagnostic Ignored; std::vector<uint8> AK, BK;
					StructuralKey(*A.Type, AK, Ignored); StructuralKey(*B.Type, BK, Ignored);
					return AK != BK ? ByteLess(AK, BK) : A.AuthoredFlags < B.AuthoredFlags;
				});
				for (size_t Index = 1; Index < Schema.Fields.size(); ++Index)
					if (Schema.Fields[Index - 1].Name == Schema.Fields[Index].Name)
						return Fail(Diagnostic, EWriterFailure::DuplicateInput, "A schema contains a duplicate field.", Schema.QualifiedName + "::" + Schema.Fields[Index].Name);
			}
			std::ranges::sort(Result.Schemas, [](const auto& A, const auto& B) { return ByteLess(A.QualifiedName, B.QualifiedName); });
			for (size_t Index = 1; Index < Result.Schemas.size(); ++Index)
				if (Result.Schemas[Index - 1].QualifiedName == Result.Schemas[Index].QualifiedName)
					return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate schema identity.", Result.Schemas[Index].QualifiedName);

			for (const auto& Type : DiscoveredTypes)
			{
				std::vector<uint8> Key;
				if (!StructuralKey(*Type, Key, Diagnostic)) return false;
				if (std::ranges::find(Result.Types, Key, &FFrozenType::Key) == Result.Types.end())
					Result.Types.push_back({Type, std::move(Key)});
			}
			if (Result.Types.size() > MaximumTableEntries)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Type count exceeds the DAST v4 bound.");
			std::ranges::sort(Result.Types, [](const auto& A, const auto& B) { return ByteLess(A.Key, B.Key); });

			Result.CustomVersions = Input.CustomVersions;
			if (Result.CustomVersions.size() > MaximumCustomVersions)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Custom version count exceeds the DAST v4 bound.");
			std::ranges::sort(Result.CustomVersions, [](const auto& A, const auto& B) {
				return std::tie(A.Guid.A, A.Guid.B, A.Guid.C, A.Guid.D)
					< std::tie(B.Guid.A, B.Guid.B, B.Guid.C, B.Guid.D);
			});
			for (size_t Index = 0; Index < Result.CustomVersions.size(); ++Index)
			{
				const auto& Custom = Result.CustomVersions[Index];
				if (Index > 0 && Custom.Guid == Result.CustomVersions[Index - 1].Guid)
					return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate custom version GUID.");
				if (Custom.EmissionValue && *Custom.EmissionValue != Custom.Value)
					return Fail(Diagnostic, EWriterFailure::ManifestMismatch, "Custom version changed after discovery.");
				if (Custom.MaximumSupported && Custom.Value > *Custom.MaximumSupported)
					return Fail(Diagnostic, EWriterFailure::UnsupportedType, "A known custom version is unsupported.");
				if (Custom.bRequiredForInterpretation && !Custom.bCodecKnown)
					return Fail(Diagnostic, EWriterFailure::UnsupportedType, "An unknown custom version is required for interpretation.");
			}

			Result.Objects = Input.Objects;
			if (Result.Objects.empty()) return Fail(Diagnostic, EWriterFailure::InvalidTopology, "A package root object is required.");
			if (Result.Objects.size() > MaximumTableEntries)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Object count exceeds the DAST v4 bound.");
			for (const auto& Object : Result.Objects)
				if (!ValidateName(Object.Path, Diagnostic, Object.Path) || !AddName(Object.ClassName, Object.Path)
					|| !AddName(Object.ObjectName, Object.Path)) return false;
			std::ranges::sort(Result.Objects, [](const auto& A, const auto& B) {
				if (A.OuterPath.empty() != B.OuterPath.empty()) return A.OuterPath.empty();
				if (A.OuterPath != B.OuterPath) return ByteLess(A.OuterPath, B.OuterPath);
				if (A.ClassName != B.ClassName) return ByteLess(A.ClassName, B.ClassName);
				return ByteLess(A.ObjectName, B.ObjectName);
			});
			for (size_t Index = 0; Index < Result.Objects.size(); ++Index)
			{
				const auto& Object = Result.Objects[Index];
				const std::string Expected = Object.OuterPath.empty() ? Object.ObjectName : Object.OuterPath + "/" + Object.ObjectName;
				if (Object.Path != Expected || (Index == 0) != Object.OuterPath.empty())
					return Fail(Diagnostic, EWriterFailure::InvalidTopology, "Object path or package root is noncanonical.", Object.Path);
				if (!Object.OuterPath.empty())
				{
					const auto Parent = std::ranges::find(std::span(Result.Objects).first(Index), Object.OuterPath, &FObjectDescriptor::Path);
					if (Parent == std::span(Result.Objects).first(Index).end())
						return Fail(Diagnostic, EWriterFailure::InvalidTopology, "An object outer is missing or follows its child.", Object.Path);
				}
				for (size_t Other = 0; Other < Index; ++Other)
					if (Result.Objects[Other].Path == Object.Path)
						return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate sibling object identity.", Object.Path);
			}

			std::ranges::sort(Names, [](const auto& A, const auto& B) { return ByteLess(A, B); });
			Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
			if (Names.size() > MaximumTableEntries)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Name count exceeds the DAST v4 bound.");
			Result.Names = std::move(Names);
			Out = std::move(Result); return true;
		}

		auto EncodeTables(const FFrozenTables& Tables, std::array<std::vector<uint8>, 4>& Out) -> void
		{
			FWireWriter Names;
			Names.VarUInt(Tables.Names.size()); for (const auto& Name : Tables.Names) Names.String(Name);
			Out[0] = Names.Take();
			FWireWriter Types; Types.VarUInt(Tables.Types.size());
			for (const auto& Frozen : Tables.Types)
			{
				const auto& Type = *Frozen.Descriptor; FWireWriter Record; Record.U8(uint8(Type.Opcode));
				switch (Type.Opcode)
				{
				case ETypeOpcode::Enum: Record.VarUInt(Tables.NameId(Type.QualifiedName)); Record.U8(uint8(Type.Parameter)); break;
				case ETypeOpcode::Intrinsic: Record.U8(uint8(Type.Parameter)); break;
				case ETypeOpcode::Struct: Record.VarUInt(Tables.NameId(Type.QualifiedName)); break;
				case ETypeOpcode::FixedArray: Record.VarUInt(Tables.TypeId(*Type.Children[0])); Record.VarUInt(Type.Parameter); break;
				case ETypeOpcode::Array: Record.VarUInt(Tables.TypeId(*Type.Children[0])); break;
				case ETypeOpcode::Map: Record.VarUInt(Tables.TypeId(*Type.Children[0])); Record.VarUInt(Tables.TypeId(*Type.Children[1])); break;
				case ETypeOpcode::HardRef: case ETypeOpcode::SoftRef: Record.VarUInt(Type.QualifiedName.empty() ? 0 : Tables.NameId(Type.QualifiedName)); break;
				default: break;
				}
				Types.Record(Record.View());
			}
			Out[1] = Types.Take();
			FWireWriter Schemas; Schemas.VarUInt(Tables.CustomVersions.size());
			for (const auto& Custom : Tables.CustomVersions)
			{
				Schemas.U32(Custom.Guid.A); Schemas.U32(Custom.Guid.B); Schemas.U32(Custom.Guid.C); Schemas.U32(Custom.Guid.D); Schemas.VarUInt(Custom.Value);
			}
			Schemas.VarUInt(Tables.Schemas.size());
			for (const auto& Schema : Tables.Schemas)
			{
				FWireWriter Record; Record.VarUInt(Tables.NameId(Schema.QualifiedName)); Record.VarUInt(Schema.Fields.size());
				for (const auto& Field : Schema.Fields)
				{
					Record.VarUInt(Tables.NameId(Field.Name)); Record.VarUInt(Tables.TypeId(*Field.Type)); Record.VarUInt(Field.AuthoredFlags);
				}
				Schemas.Record(Record.View());
			}
			Out[2] = Schemas.Take();
			FWireWriter Objects; Objects.VarUInt(Tables.Objects.size());
			for (const auto& Object : Tables.Objects)
			{
				FWireWriter Record; Record.VarUInt(Object.OuterPath.empty() ? 0 : Tables.ObjectId(Object.OuterPath));
				Record.VarUInt(Tables.NameId(Object.ClassName)); Record.VarUInt(Tables.NameId(Object.ObjectName)); Objects.Record(Record.View());
			}
			Out[3] = Objects.Take();
		}

		auto SignedFits(ETypeOpcode Opcode, int64 Value) -> bool
		{
			const uint32 Bits = Opcode == ETypeOpcode::I8 ? 8 : Opcode == ETypeOpcode::I16 ? 16 : Opcode == ETypeOpcode::I32 ? 32 : 64;
			return Bits == 64 || (Value >= -(int64(1) << (Bits - 1)) && Value <= (int64(1) << (Bits - 1)) - 1);
		}
		auto UnsignedFits(ETypeOpcode Opcode, uint64 Value) -> bool
		{
			const uint32 Bits = Opcode == ETypeOpcode::U8 ? 8 : Opcode == ETypeOpcode::U16 ? 16 : Opcode == ETypeOpcode::U32 ? 32 : 64;
			return Bits == 64 || Value <= ((uint64(1) << Bits) - 1);
		}

		auto EncodeValue(const FTypeDescriptor& Type, const FValue& Value,
			const FFrozenTables& Tables, FWireWriter& Writer, uint32 Depth,
			FWriterDiagnostic& Diagnostic, std::string_view Path) -> bool
		{
			if (Depth > MaximumValueDepth)
				return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Value nesting exceeds the DAST v4 bound.", std::string(Path));
			switch (Type.Opcode)
			{
			case ETypeOpcode::Bool: Writer.U8(Value.Bool ? 1 : 0); return true;
			case ETypeOpcode::I8: case ETypeOpcode::I16: case ETypeOpcode::I32: case ETypeOpcode::I64:
				if (!SignedFits(Type.Opcode, Value.Signed)) return Fail(Diagnostic, EWriterFailure::InvalidValue, "A signed integer is out of range.", std::string(Path));
				Writer.VarInt(Value.Signed); return true;
			case ETypeOpcode::U8: case ETypeOpcode::U16: case ETypeOpcode::U32: case ETypeOpcode::U64:
				if (!UnsignedFits(Type.Opcode, Value.Unsigned)) return Fail(Diagnostic, EWriterFailure::InvalidValue, "An unsigned integer is out of range.", std::string(Path));
				Writer.VarUInt(Value.Unsigned); return true;
			case ETypeOpcode::F32:
			{
				uint32 Bits = uint32(Value.FloatingBits);
				if (std::isnan(std::bit_cast<float>(Bits))) Bits = 0x7fc00000u;
				Writer.U32(Bits); return true;
			}
			case ETypeOpcode::F64:
			{
				uint64 Bits = Value.FloatingBits;
				if (std::isnan(std::bit_cast<double>(Bits))) Bits = 0x7ff8000000000000ull;
				Writer.U64(Bits); return true;
			}
			case ETypeOpcode::String:
				if (Value.Text.size() > MaximumStringBytes) return Fail(Diagnostic, EWriterFailure::LimitExceeded, "A string value exceeds the bound.", std::string(Path));
				if (!IsValidUtf8(Value.Text) && !Value.Text.empty()) return Fail(Diagnostic, EWriterFailure::InvalidUtf8, "A string value is invalid UTF-8.", std::string(Path));
				Writer.String(Value.Text); return true;
			case ETypeOpcode::Name:
			{
				const uint64 Id = Tables.NameId(Value.Text);
				if (Id == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A Name value was not discovered.", std::string(Path));
				Writer.VarUInt(Id); return true;
			}
			case ETypeOpcode::Guid:
				Writer.U32(Value.Guid.A); Writer.U32(Value.Guid.B); Writer.U32(Value.Guid.C); Writer.U32(Value.Guid.D); return true;
			case ETypeOpcode::Enum:
				if (Type.Parameter <= uint8(ETypeOpcode::I64))
				{
					if (!SignedFits(ETypeOpcode(Type.Parameter), Value.Signed)) return Fail(Diagnostic, EWriterFailure::InvalidValue, "A signed enum is out of range.", std::string(Path));
					Writer.VarInt(Value.Signed);
				}
				else
				{
					if (!UnsignedFits(ETypeOpcode(Type.Parameter), Value.Unsigned)) return Fail(Diagnostic, EWriterFailure::InvalidValue, "An unsigned enum is out of range.", std::string(Path));
					Writer.VarUInt(Value.Unsigned);
				}
				return true;
			case ETypeOpcode::Intrinsic:
			{
				const uint64 Count = Type.Parameter == 1 ? 2 : Type.Parameter == 2 ? 3 : Type.Parameter == 5 ? 10 : 4;
				if (Value.ComponentBits.size() != Count) return Fail(Diagnostic, EWriterFailure::InvalidValue, "Intrinsic component count is invalid.", std::string(Path));
				for (uint64 Bits : Value.ComponentBits)
				{
					if (Type.Parameter == 6)
					{
						uint32 B = uint32(Bits); if (std::isnan(std::bit_cast<float>(B))) B = 0x7fc00000u; Writer.U32(B);
					}
					else { if (std::isnan(std::bit_cast<double>(Bits))) Bits = 0x7ff8000000000000ull; Writer.U64(Bits); }
				}
				return true;
			}
			case ETypeOpcode::Struct:
			{
				if (Type.bHasCustomSerializer) return Fail(Diagnostic, EWriterFailure::UnsupportedType, "A Struct custom serializer has no DAST v4 codec.", std::string(Path));
				if (!Type.bHasDeterministicStructOperations) return Fail(Diagnostic, EWriterFailure::UnsupportedType, "Deterministic Struct operations are unavailable.", std::string(Path));
				const uint64 Schema = Tables.SchemaId(Type.QualifiedName);
				if (Schema == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A Struct schema was not discovered.", std::string(Path));
				if (Value.FieldNames.size() != Value.Elements.size() || Value.Provenances.size() != Value.Elements.size())
					return Fail(Diagnostic, EWriterFailure::InvalidValue, "Struct field arrays have mismatched lengths.", std::string(Path));
				struct FEncoded { uint64 Id; EDefaultDeltaProvenance Provenance; std::vector<uint8> Bytes; };
				std::vector<FEncoded> Fields;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const uint64 Id = Tables.FieldId(Schema, Value.FieldNames[Index]);
					if (Id == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A Struct field was not discovered.", std::string(Path));
					const auto Provenance = Value.Provenances[Index];
					if (Provenance != EDefaultDeltaProvenance::Explicit && Provenance != EDefaultDeltaProvenance::Forced)
						return Fail(Diagnostic, EWriterFailure::InvalidValue, "Struct provenance is invalid.", std::string(Path));
					FWireWriter Child;
					if (!EncodeValue(*Tables.Schemas[Schema - 1].Fields[Id - 1].Type, Value.Elements[Index], Tables, Child, Depth + 1, Diagnostic, Path)) return false;
					Fields.push_back({Id, Provenance, Child.Take()});
				}
				std::ranges::sort(Fields, {}, &FEncoded::Id);
				for (size_t Index = 1; Index < Fields.size(); ++Index) if (Fields[Index - 1].Id == Fields[Index].Id)
					return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate Struct field override.", std::string(Path));
				Writer.VarUInt(Fields.size());
				for (const auto& Field : Fields) { Writer.VarUInt(Field.Id); Writer.U8(Field.Provenance == EDefaultDeltaProvenance::Forced ? 1 : 0); Writer.Record(Field.Bytes); }
				return true;
			}
			case ETypeOpcode::FixedArray:
				if (Value.Elements.size() != Type.Parameter) return Fail(Diagnostic, EWriterFailure::InvalidValue, "FixedArray element count is invalid.", std::string(Path));
				for (const auto& Element : Value.Elements) if (!EncodeValue(*Type.Children[0], Element, Tables, Writer, Depth + 1, Diagnostic, Path)) return false;
				return true;
			case ETypeOpcode::Array:
				if (Value.Elements.size() > MaximumContainerElements) return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Array count exceeds the bound.", std::string(Path));
				Writer.VarUInt(Value.Elements.size());
				for (const auto& Element : Value.Elements) if (!EncodeValue(*Type.Children[0], Element, Tables, Writer, Depth + 1, Diagnostic, Path)) return false;
				return true;
			case ETypeOpcode::Map:
			{
				if (Value.Elements.size() % 2 != 0 || Value.Elements.size() / 2 > MaximumContainerElements)
					return Fail(Diagnostic, EWriterFailure::InvalidValue, "Map element count is invalid.", std::string(Path));
				struct FEntry { std::vector<uint8> Key; std::vector<uint8> Value; };
				std::vector<FEntry> Entries;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FWireWriter Key, MapValue;
					if (!EncodeValue(*Type.Children[0], Value.Elements[Index], Tables, Key, Depth + 1, Diagnostic, Path)
						|| !EncodeValue(*Type.Children[1], Value.Elements[Index + 1], Tables, MapValue, Depth + 1, Diagnostic, Path)) return false;
					Entries.push_back({Key.Take(), MapValue.Take()});
				}
				std::ranges::sort(Entries, [](const auto& A, const auto& B) { return ByteLess(A.Key, B.Key); });
				for (size_t Index = 1; Index < Entries.size(); ++Index) if (Entries[Index - 1].Key == Entries[Index].Key)
					return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate logical Map key.", std::string(Path));
				Writer.VarUInt(Entries.size()); for (const auto& Entry : Entries) { Writer.Bytes(Entry.Key); Writer.Bytes(Entry.Value); } return true;
			}
			case ETypeOpcode::HardRef:
				if (Value.ReferenceTag > 2 || (Value.ReferenceTag != 0 && (Value.ReferenceId == 0
					|| (Value.ReferenceTag == 1 && Value.ReferenceId > Tables.Objects.size())
					|| (Value.ReferenceTag == 2 && Value.ReferenceId > Tables.DependencyCount))))
					return Fail(Diagnostic, EWriterFailure::InvalidValue, "Hard reference tag or id is invalid.", std::string(Path));
				Writer.U8(Value.ReferenceTag); if (Value.ReferenceTag != 0) Writer.VarUInt(Value.ReferenceId); return true;
			case ETypeOpcode::SoftRef:
				if (Value.ReferenceTag > 1) return Fail(Diagnostic, EWriterFailure::InvalidValue, "Soft reference tag is invalid.", std::string(Path));
				Writer.U8(Value.ReferenceTag);
				if (Value.ReferenceTag == 1) { const uint64 Id = Tables.NameId(Value.Text); if (Id == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A soft reference path was not discovered.", std::string(Path)); Writer.VarUInt(Id); }
				return true;
			case ETypeOpcode::Bytes:
				if (Value.Bytes.size() > MaximumContainerElements) return Fail(Diagnostic, EWriterFailure::LimitExceeded, "Byte value exceeds the bound.", std::string(Path));
				Writer.VarUInt(Value.Bytes.size()); Writer.Bytes(Value.Bytes); return true;
			}
			return Fail(Diagnostic, EWriterFailure::UnsupportedType, "A value opcode is unsupported.", std::string(Path));
		}

		auto ValidateRetainedClosure(std::span<const uint8> Closure) -> bool
		{
			FWireReader Reader(Closure);
			std::array<std::span<const uint8>, 3> Sections;
			for (auto& Section : Sections) if (!Reader.Record(Section) || Section.empty()) return false;
			uint64 RootSchema = 0, RootField = 0;
			if (!Reader.VarUInt(RootSchema) || !Reader.VarUInt(RootField) || !Reader.End()
				|| RootSchema == 0 || RootField == 0) return false;

			std::vector<std::string> Names;
			FWireReader NameReader(Sections[0]);
			uint64 NameCount = 0;
			if (!NameReader.VarUInt(NameCount) || NameCount > MaximumTableEntries) return false;
			for (uint64 Index = 0; Index < NameCount; ++Index)
			{
				std::string Name;
				if (!NameReader.String(Name) || Name.empty()
					|| (!Names.empty() && !ByteLess(Names.back(), Name))) return false;
				Names.push_back(std::move(Name));
			}
			if (!NameReader.End()) return false;

			struct FDecodedType
			{
				ETypeOpcode Opcode = ETypeOpcode::Bool;
				uint64 NameId = 0;
				uint64 Parameter = 0;
				std::vector<uint64> Children;
			};
			std::vector<FDecodedType> Types;
			FWireReader TypeReader(Sections[1]);
			uint64 TypeCount = 0;
			if (!TypeReader.VarUInt(TypeCount) || TypeCount > MaximumTableEntries) return false;
			for (uint64 Index = 0; Index < TypeCount; ++Index)
			{
				std::span<const uint8> RecordBytes;
				if (!TypeReader.Record(RecordBytes)) return false;
				FWireReader Record(RecordBytes); uint8 RawOpcode = 0;
				if (!Record.U8(RawOpcode) || RawOpcode < uint8(ETypeOpcode::Bool)
					|| RawOpcode > uint8(ETypeOpcode::Bytes)) return false;
				FDecodedType Type{.Opcode = ETypeOpcode(RawOpcode)};
				switch (Type.Opcode)
				{
				case ETypeOpcode::Enum:
				{ uint8 Storage = 0; if (!Record.VarUInt(Type.NameId) || Type.NameId == 0 || Type.NameId > Names.size()
					|| !Record.U8(Storage) || Storage < uint8(ETypeOpcode::I8) || Storage > uint8(ETypeOpcode::U64)) return false; Type.Parameter = Storage; break; }
				case ETypeOpcode::Intrinsic:
				{ uint8 Layout = 0; if (!Record.U8(Layout) || Layout < 1 || Layout > 6) return false; Type.Parameter = Layout; break; }
				case ETypeOpcode::Struct:
					if (!Record.VarUInt(Type.NameId) || Type.NameId == 0 || Type.NameId > Names.size()) return false; break;
				case ETypeOpcode::FixedArray:
				{ uint64 Child = 0; if (!Record.VarUInt(Child) || !Record.VarUInt(Type.Parameter)
					|| Child == 0 || Child > TypeCount || Type.Parameter == 0 || Type.Parameter > MaximumContainerElements) return false; Type.Children = {Child}; break; }
				case ETypeOpcode::Array:
				{ uint64 Child = 0; if (!Record.VarUInt(Child) || Child == 0 || Child > TypeCount) return false; Type.Children = {Child}; break; }
				case ETypeOpcode::Map:
				{ uint64 Key = 0, Value = 0; if (!Record.VarUInt(Key) || !Record.VarUInt(Value)
					|| Key == 0 || Key > TypeCount || Value == 0 || Value > TypeCount) return false; Type.Children = {Key, Value}; break; }
				case ETypeOpcode::HardRef: case ETypeOpcode::SoftRef:
					if (!Record.VarUInt(Type.NameId) || Type.NameId > Names.size()) return false; break;
				default: break;
				}
				if (!Record.End()) return false;
				Types.push_back(std::move(Type));
			}
			if (!TypeReader.End()) return false;
			std::vector<std::vector<uint8>> TypeKeys(Types.size());
			std::vector<uint8> State(Types.size());
			std::function<bool(uint64)> BuildKey = [&](uint64 Id) {
				const size_t Index = size_t(Id - 1);
				if (State[Index] == 2) return true;
				if (State[Index] == 1) return false;
				State[Index] = 1; const auto& Type = Types[Index]; FWireWriter Key; Key.U8(uint8(Type.Opcode));
				if (Type.Opcode == ETypeOpcode::Enum) { Key.String(Names[Type.NameId - 1]); Key.U8(uint8(Type.Parameter)); }
				else if (Type.Opcode == ETypeOpcode::Intrinsic) Key.U8(uint8(Type.Parameter));
				else if (Type.Opcode == ETypeOpcode::Struct) Key.String(Names[Type.NameId - 1]);
				else if (Type.Opcode == ETypeOpcode::HardRef || Type.Opcode == ETypeOpcode::SoftRef)
					Key.String(Type.NameId == 0 ? std::string_view{} : Names[Type.NameId - 1]);
				else if (Type.Opcode == ETypeOpcode::FixedArray) Key.VarUInt(Type.Parameter);
				for (uint64 Child : Type.Children)
				{
					if (!BuildKey(Child)) return false; Key.Bytes(TypeKeys[Child - 1]);
				}
				TypeKeys[Index] = Key.Take(); State[Index] = 2; return true;
			};
			for (uint64 Id = 1; Id <= Types.size(); ++Id) if (!BuildKey(Id)) return false;
			for (size_t Index = 1; Index < TypeKeys.size(); ++Index)
				if (!ByteLess(TypeKeys[Index - 1], TypeKeys[Index])) return false;

			FWireReader SchemaReader(Sections[2]);
			uint64 CustomVersions = 0, SchemaCount = 0;
			if (!SchemaReader.VarUInt(CustomVersions) || CustomVersions != 0
				|| !SchemaReader.VarUInt(SchemaCount) || SchemaCount > MaximumTableEntries
				|| RootSchema > SchemaCount) return false;
			uint64 PreviousSchemaName = 0;
			for (uint64 SchemaIndex = 1; SchemaIndex <= SchemaCount; ++SchemaIndex)
			{
				std::span<const uint8> RecordBytes; if (!SchemaReader.Record(RecordBytes)) return false;
				FWireReader Record(RecordBytes); uint64 SchemaName = 0, FieldCount = 0;
				if (!Record.VarUInt(SchemaName) || SchemaName == 0 || SchemaName > Names.size()
					|| SchemaName <= PreviousSchemaName || !Record.VarUInt(FieldCount)
					|| FieldCount > MaximumSchemaFields || (SchemaIndex == RootSchema && RootField > FieldCount)) return false;
				uint64 PreviousFieldName = 0;
				for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
				{
					uint64 FieldName = 0, TypeId = 0, Flags = 0;
					if (!Record.VarUInt(FieldName) || FieldName == 0 || FieldName > Names.size()
						|| FieldName <= PreviousFieldName || !Record.VarUInt(TypeId)
						|| TypeId == 0 || TypeId > Types.size() || !Record.VarUInt(Flags) || Flags != 0) return false;
					PreviousFieldName = FieldName;
				}
				if (!Record.End()) return false; PreviousSchemaName = SchemaName;
			}
			return SchemaReader.End();
		}

		auto EncodeValues(const FPackageInput& Input, const FFrozenTables& Tables,
			std::vector<uint8>& Out, FWriterDiagnostic& Diagnostic) -> bool
		{
			if (Input.ObjectValues.size() != Tables.Objects.size())
				return Fail(Diagnostic, EWriterFailure::ManifestMismatch, "Value-section object count differs from the frozen object table.");
			std::vector<const FObjectValueInput*> ById(Tables.Objects.size());
			for (const auto& Object : Input.ObjectValues)
			{
				const uint64 Id = Tables.ObjectId(Object.ObjectPath);
				if (Id == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A value-section object was not discovered.", Object.ObjectPath);
				if (ById[Id - 1]) return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate value-section object.", Object.ObjectPath);
				ById[Id - 1] = &Object;
			}
			FWireWriter Section; Section.VarUInt(ById.size());
			for (const auto* Object : ById)
			{
				if (!Object) return Fail(Diagnostic, EWriterFailure::ManifestMismatch, "A frozen object has no value block.");
				struct FOverride { uint64 Schema; uint64 Field; uint8 Provenance; std::vector<uint8> Bytes; };
				std::vector<FOverride> Overrides;
				for (const auto& Known : Object->KnownOverrides)
				{
					const uint64 Schema = Tables.SchemaId(Known.SchemaName), Field = Tables.FieldId(Schema, Known.FieldName);
					if (Schema == 0 || Field == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "An override field was not discovered.", Known.SchemaName + "::" + Known.FieldName);
					if (Known.Provenance != EDefaultDeltaProvenance::Explicit && Known.Provenance != EDefaultDeltaProvenance::Forced)
						return Fail(Diagnostic, EWriterFailure::InvalidValue, "Known override provenance is invalid.", Known.SchemaName + "::" + Known.FieldName);
					FWireWriter Value;
					if (!EncodeValue(*Tables.Schemas[Schema - 1].Fields[Field - 1].Type, Known.Value, Tables, Value, 0, Diagnostic, Known.SchemaName + "::" + Known.FieldName)) return false;
					Overrides.push_back({Schema, Field, Known.Provenance == EDefaultDeltaProvenance::Forced ? uint8(1) : uint8(0), Value.Take()});
				}
				for (const auto& Unknown : Object->RetainedUnknownOverrides)
				{
					const uint64 Schema = Tables.SchemaId(Unknown.SchemaName), Field = Tables.FieldId(Schema, Unknown.FieldName);
					if (Schema == 0 || Field == 0) return Fail(Diagnostic, EWriterFailure::MissingDiscovery, "A retained override field was not discovered.", Unknown.SchemaName + "::" + Unknown.FieldName);
					if (!ValidateRetainedClosure(Unknown.DescriptorClosure)) return Fail(Diagnostic, EWriterFailure::InvalidRetainedClosure, "A retained descriptor closure is malformed.", Unknown.SchemaName + "::" + Unknown.FieldName);
					FWireWriter Body; Body.Record(Unknown.DescriptorClosure); Body.Record(Unknown.Payload);
					Overrides.push_back({Schema, Field, 2, Body.Take()});
				}
				std::ranges::sort(Overrides, [](const auto& A, const auto& B) { return A.Schema != B.Schema ? A.Schema < B.Schema : A.Field < B.Field; });
				for (size_t Index = 1; Index < Overrides.size(); ++Index)
					if (Overrides[Index - 1].Schema == Overrides[Index].Schema && Overrides[Index - 1].Field == Overrides[Index].Field)
						return Fail(Diagnostic, EWriterFailure::DuplicateInput, "Duplicate object override.", Object->ObjectPath);
				FWireWriter Block; Block.VarUInt(Overrides.size());
				for (const auto& Override : Overrides) { Block.VarUInt(Override.Schema); Block.VarUInt(Override.Field); Block.U8(Override.Provenance); Block.Record(Override.Bytes); }
				Section.Record(Block.View());
			}
			Out = Section.Take(); return true;
		}
	}

	auto MakeType(ETypeOpcode Opcode, std::string QualifiedName, uint64 Parameter,
		std::vector<FTypePtr> Children) -> FTypePtr
	{
		return std::make_shared<FTypeDescriptor>(FTypeDescriptor{
			.Opcode = Opcode, .QualifiedName = std::move(QualifiedName),
			.Parameter = Parameter, .Children = std::move(Children)});
	}

	auto WritePackage(const FPackageInput& Input, std::vector<uint8>& OutBytes,
		FWriterDiagnostic* OutDiagnostic) -> bool
	{
		FWriterDiagnostic Diagnostic;
		auto Finish = [&](bool Result) { if (OutDiagnostic) *OutDiagnostic = Diagnostic; return Result; };
		if (!ValidateName(Input.AssetClass, Diagnostic, "Summary.AssetClass")) return Finish(false);
		if (Input.EntryKind != EAssetRegistryEntryKind::Asset && Input.EntryKind != EAssetRegistryEntryKind::Redirector)
			return Finish(Fail(Diagnostic, EWriterFailure::InvalidInput, "Public-summary entry kind is invalid."));
		if ((Input.EntryKind == EAssetRegistryEntryKind::Asset) != Input.RedirectDestination.empty())
			return Finish(Fail(Diagnostic, EWriterFailure::InvalidInput, "Redirect destination does not match the entry kind."));
		if (!Input.RedirectDestination.empty() && !ValidateName(Input.RedirectDestination, Diagnostic, "Summary.RedirectDestination")) return Finish(false);
		std::vector<std::string> Dependencies = Input.Dependencies;
		if (Dependencies.size() > MaximumDependencies)
			return Finish(Fail(Diagnostic, EWriterFailure::LimitExceeded,
				"Dependency count exceeds the DAST v4 bound."));
		for (const auto& Dependency : Dependencies) if (!ValidateName(Dependency, Diagnostic, Dependency)) return Finish(false);
		std::ranges::sort(Dependencies, [](const auto& A, const auto& B) { return ByteLess(A, B); });
		if (std::adjacent_find(Dependencies.begin(), Dependencies.end()) != Dependencies.end())
			return Finish(Fail(Diagnostic, EWriterFailure::DuplicateInput, "Public dependencies contain a duplicate."));
		if (Dependencies != Input.Dependencies)
			return Finish(Fail(Diagnostic, EWriterFailure::InvalidInput, "Public dependencies are not in canonical byte order."));

		FFrozenTables Tables;
		if (!FreezeTables(Input, Tables, Diagnostic)) return Finish(false);
		std::array<std::vector<uint8>, 4> TableSections;
		EncodeTables(Tables, TableSections);
		std::vector<uint8> Values;
		if (!EncodeValues(Input, Tables, Values, Diagnostic)) return Finish(false);
		std::array<std::vector<uint8>, SectionCount> Sections{
			TableSections[0], TableSections[1], TableSections[2], TableSections[3], std::move(Values)};

		FWireWriter Summary; Summary.String(Input.AssetClass); Summary.U8(uint8(Input.EntryKind));
		Summary.String(Input.RedirectDestination); Summary.VarUInt(Dependencies.size());
		for (const auto& Dependency : Dependencies) Summary.String(Dependency);
		Summary.VarUInt(Tables.Objects.size());
		if (Summary.View().size() > MaximumSummaryBytes)
			return Finish(Fail(Diagnostic, EWriterFailure::LimitExceeded, "Public summary exceeds the DAST v4 bound."));
		uint64 Total = 4 + 4 + 4 + 1 + Summary.View().size() + SectionCount * 9;
		for (const auto& Section : Sections)
		{
			if (Section.size() > std::numeric_limits<uint32>::max() || Total > MaximumPackageBytes - Section.size())
				return Finish(Fail(Diagnostic, EWriterFailure::PackageTooLarge, "DAST v4 package exceeds its bound."));
			Total += Section.size();
		}
		FWireWriter Result; Result.U32(Magic); Result.U32(Version); Result.U32(uint32(Summary.View().size()));
		Result.U8(SectionCount); Result.Bytes(Summary.View());
		uint32 Offset = uint32(4 + 4 + 4 + 1 + Summary.View().size() + SectionCount * 9);
		for (uint8 Index = 0; Index < SectionCount; ++Index)
		{
			Result.U8(Index + 1); Result.U32(Offset); Result.U32(uint32(Sections[Index].size())); Offset += uint32(Sections[Index].size());
		}
		for (const auto& Section : Sections) Result.Bytes(Section);
		std::vector<uint8> Complete = Result.Take();
		if (Complete.size() != Total) return Finish(Fail(Diagnostic, EWriterFailure::ManifestMismatch, "Final package extent differs from the frozen manifest."));
		OutBytes = std::move(Complete);
		Diagnostic.Reset(); return Finish(true);
	}
}
