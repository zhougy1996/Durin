#include "DObject/DefaultDeltaPlan.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	namespace
	{
		using ETypeKind = FArchiveLogicalTypeDescriptor::EKind;

		struct FCaptureState
		{
			bool bCountSeen = false;
			uint64 ExpectedCount = 0;
			uint64 NextImplicitElement = 0;
		};

		auto FieldLess(const FDefaultDeltaFieldPlan& Left, const FDefaultDeltaFieldPlan& Right) -> bool
		{
			return std::pair(
				Left.Descriptor.DeclaringType.ToString(), Left.Descriptor.Name.ToString())
				< std::pair(
					Right.Descriptor.DeclaringType.ToString(), Right.Descriptor.Name.ToString());
		}

		auto DescriptorEquivalent(
			const FArchiveFieldDescriptor& Left,
			const FArchiveFieldDescriptor& Right) -> bool
		{
			return Left.DeclaringType == Right.DeclaringType
				&& Left.Name == Right.Name
				&& Left.ArrayDimension == Right.ArrayDimension
				&& Left.PropertyFlags == Right.PropertyFlags
				&& AreArchiveLogicalTypesEquivalent(Left.LogicalType, Right.LogicalType);
		}

		class FLogicalValueCaptureArchive final : public FObjectArchive
		{
		public:
			explicit FLogicalValueCaptureArchive(EArchivePurpose Purpose)
				: FObjectArchive({EArchiveDirection::Save, Purpose,
					EArchiveCapability::StructuredFields
					| EArchiveCapability::RawBytes
					| EArchiveCapability::CanonicalMapOrder
					| EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences
					| EArchiveCapability::MultiPassDiscovery})
			{
			}

			std::vector<FDefaultDeltaFieldPlan> RootFields;

			auto Finish() -> bool
			{
				if (HasError()) return false;
				for (const auto& [Node, State] : CaptureStates)
				{
					if (!Node) continue;
					if ((Node->LogicalType.Kind == ETypeKind::Array && State.bCountSeen
							&& Node->Elements.size() != State.ExpectedCount)
						|| (Node->LogicalType.Kind == ETypeKind::Map && State.bCountSeen
							&& Node->Elements.size() != State.ExpectedCount * 2)
						|| (Node->LogicalType.Kind == ETypeKind::FixedArray
							&& Node->Elements.size() != Node->LogicalType.FixedArrayDimension))
					{
						Fail(EArchiveFailureCode::MalformedSerializer,
							"Logical container capture count does not match its descriptor or authored count.");
						return false;
					}
				}
				return true;
			}

			auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node || Node->LogicalType.Kind != ETypeKind::Bytes)
				{
					Fail(EArchiveFailureCode::UnsupportedType,
						"Raw logical capture is valid only for an authored Bytes field.");
					return;
				}
				Node->ByteValue.insert(Node->ByteValue.end(), Bytes.begin(), Bytes.end());
				Node->bHasAtomicValue = true;
			}

			auto SerializeBulkData(FArchiveBulkDataTransfer& Value) -> void override
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node || Node->LogicalType.Kind != ETypeKind::BulkData)
				{
					Fail(EArchiveFailureCode::UnsupportedType,
						"Bulk data does not match the active logical descriptor.");
					return;
				}
				if (Value.LogicalSize != Value.StoredSize)
				{
					Fail(EArchiveFailureCode::InvalidData,
						"Bulk data logical identity requires matching logical and stored sizes.");
					return;
				}
				const auto Append = [&Node](const auto& Part) {
					const auto Bytes = std::as_bytes(std::span{&Part, 1});
					Node->ByteValue.insert(Node->ByteValue.end(), Bytes.begin(), Bytes.end());
				};
				Append(Value.LogicalSize);
				Append(Value.ContentHash);
				Node->bHasAtomicValue = true;
			}

			auto SerializeObjectReference(DObject*& Value) -> void override
			{
				FDefaultDeltaNode* Node = ResolveAtomicTarget();
				if (!Node || Node->LogicalType.Kind != ETypeKind::Object)
				{
					Fail(EArchiveFailureCode::UnsupportedType,
						"Object reference does not match the active logical descriptor.");
					return;
				}
				Node->ObjectValue = Value;
				Node->bHasAtomicValue = true;
			}

			auto SerializeSoftObjectPath(FSoftObjectPath& Value) -> void override
			{
				FDefaultDeltaNode* Node = ResolveAtomicTarget();
				if (!Node || Node->LogicalType.Kind != ETypeKind::SoftObject)
				{
					Fail(EArchiveFailureCode::UnsupportedType,
						"Soft reference does not match the active logical descriptor.");
					return;
				}
				Node->TextValue = Value.IsNull() ? std::string{} : Value.ToString();
				Node->bHasAtomicValue = true;
			}

		protected:
			auto OnReflectedPropertyValue(
				FProperty& Property,
				const void* Container,
				uint32 ArrayIndex) -> void override
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node) return;
				Node->SourceValue = Property.GetValuePtr(Container, ArrayIndex);
				Node->SourceStruct = Property.GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
					? static_cast<FStructProperty&>(Property).GetStruct() : nullptr;
			}

			auto OnEnterField(const FArchiveFieldDescriptor& Descriptor) -> void override
			{
				auto Value = std::make_shared<FDefaultDeltaNode>();
				Value->LogicalType = Descriptor.LogicalType;
				FDefaultDeltaFieldPlan Field{
					.Descriptor = Descriptor,
					.Value = Value};
				if (FDefaultDeltaNode* Parent = CurrentNode())
				{
					if (Parent->LogicalType.Kind != ETypeKind::Struct)
					{
						Fail(EArchiveFailureCode::MalformedSerializer,
							"Nested named fields require an active Struct descriptor.");
						return;
					}
					Parent->Fields.push_back(std::move(Field));
					NodeStack.push_back(Parent->Fields.back().Value.get());
				}
				else
				{
					RootFields.push_back(std::move(Field));
					NodeStack.push_back(RootFields.back().Value.get());
				}
			}

			auto OnLeaveField() -> void override
			{
				if (!NodeStack.empty()) NodeStack.pop_back();
			}

			auto OnEnterFixedArrayElement(uint64 Index) -> void override
			{
				PushContainerElement(Index, false);
			}

			auto OnEnterArrayElement(uint64 Index) -> void override
			{
				PushContainerElement(Index, false);
			}

			auto OnEnterMapKey(uint64 Index) -> void override
			{
				PushContainerElement(Index, false);
			}

			auto OnEnterMapValue(uint64 Index) -> void override
			{
				PushContainerElement(Index, true);
			}

			auto OnCanonicalMapKey(uint64 Index, std::span<const std::byte> Token) -> void override
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node || Node->LogicalType.Kind != ETypeKind::Map)
				{
					Fail(EArchiveFailureCode::MalformedSerializer,
						"Canonical Map key token has no active Map value.");
					return;
				}
				MapKeyTokens[Node][Index].assign(Token.begin(), Token.end());
			}

			auto OnLeavePath() -> void override
			{
				if (!NodeStack.empty()) NodeStack.pop_back();
			}

			auto TryCaptureLogicalPrimitive(
				EArchiveLogicalPrimitiveKind Kind,
				const void* Value) -> bool override
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node) return false;
				if ((Node->LogicalType.Kind == ETypeKind::Array
						|| Node->LogicalType.Kind == ETypeKind::Map)
					&& !CaptureStates[Node].bCountSeen)
				{
					if (Kind != EArchiveLogicalPrimitiveKind::UInt64)
					{
						Fail(EArchiveFailureCode::MalformedSerializer,
							"Array and Map capture must begin with an unsigned 64-bit element count.");
						return true;
					}
					auto& State = CaptureStates[Node];
					State.bCountSeen = true;
					State.ExpectedCount = *static_cast<const uint64*>(Value);
					if (State.ExpectedCount > DefaultDeltaMaxFields)
						Fail(EArchiveFailureCode::InvalidData, "Logical container exceeds the planner element bound.");
					return true;
				}
				Node = ResolveAtomicTarget();
				if (!Node) return true;
				if (Kind == EArchiveLogicalPrimitiveKind::Bool)
				{
					Node->BoolValue = *static_cast<const bool*>(Value);
					Node->UnsignedValue = Node->BoolValue ? 1 : 0;
				}
				else if (Kind >= EArchiveLogicalPrimitiveKind::Int8
					&& Kind <= EArchiveLogicalPrimitiveKind::Int64)
				{
					switch (Kind)
					{
					case EArchiveLogicalPrimitiveKind::Int8: Node->SignedValue = *static_cast<const int8*>(Value); break;
					case EArchiveLogicalPrimitiveKind::Int16: Node->SignedValue = *static_cast<const int16*>(Value); break;
					case EArchiveLogicalPrimitiveKind::Int32: Node->SignedValue = *static_cast<const int32*>(Value); break;
					case EArchiveLogicalPrimitiveKind::Int64: Node->SignedValue = *static_cast<const int64*>(Value); break;
					default: break;
					}
				}
				else if (Kind >= EArchiveLogicalPrimitiveKind::UInt8
					&& Kind <= EArchiveLogicalPrimitiveKind::UInt64)
				{
					switch (Kind)
					{
					case EArchiveLogicalPrimitiveKind::UInt8: Node->UnsignedValue = *static_cast<const uint8*>(Value); break;
					case EArchiveLogicalPrimitiveKind::UInt16: Node->UnsignedValue = *static_cast<const uint16*>(Value); break;
					case EArchiveLogicalPrimitiveKind::UInt32: Node->UnsignedValue = *static_cast<const uint32*>(Value); break;
					case EArchiveLogicalPrimitiveKind::UInt64: Node->UnsignedValue = *static_cast<const uint64*>(Value); break;
					default: break;
					}
				}
				else if (Kind == EArchiveLogicalPrimitiveKind::Float32)
					Node->FloatingBits = std::bit_cast<uint32>(*static_cast<const float*>(Value));
				else if (Kind == EArchiveLogicalPrimitiveKind::Float64)
					Node->FloatingBits = std::bit_cast<uint64>(*static_cast<const double*>(Value));
				else if (Kind == EArchiveLogicalPrimitiveKind::Guid)
					Node->GuidValue = *static_cast<const FGuid*>(Value);
				Node->bHasAtomicValue = true;
				return true;
			}

			auto TryCaptureLogicalText(
				EArchiveLogicalTextKind Kind,
				std::string_view Value) -> bool override
			{
				FDefaultDeltaNode* Node = ResolveAtomicTarget();
				if (!Node) return CurrentNode() != nullptr;
				const ETypeKind Expected = Kind == EArchiveLogicalTextKind::String
					? ETypeKind::String : ETypeKind::Name;
				if (Node->LogicalType.Kind != Expected)
				{
					Fail(EArchiveFailureCode::UnsupportedType,
						"Text value does not match the active logical descriptor.");
					return true;
				}
				Node->TextValue.assign(Value);
				Node->bHasAtomicValue = true;
				return true;
			}

		private:
			std::vector<FDefaultDeltaNode*> NodeStack;
			std::unordered_map<FDefaultDeltaNode*, FCaptureState> CaptureStates;
			std::unordered_map<FDefaultDeltaNode*,
				std::unordered_map<uint64, std::vector<std::byte>>> MapKeyTokens;

			auto CurrentNode() -> FDefaultDeltaNode*
			{
				return NodeStack.empty() ? nullptr : NodeStack.back();
			}

			auto MakeElement(FDefaultDeltaNode* Parent, uint64 Slot) -> FDefaultDeltaNode*
			{
				if (!Parent || Slot >= DefaultDeltaMaxFields)
				{
					Fail(EArchiveFailureCode::InvalidData, "Logical element slot exceeds the planner bound.");
					return nullptr;
				}
				const FArchiveLogicalTypeDescriptor* ElementType = nullptr;
				if (Parent->LogicalType.Kind == ETypeKind::Map)
					ElementType = Slot % 2 == 0 ? Parent->LogicalType.KeyType.get() : Parent->LogicalType.ValueType.get();
				else ElementType = Parent->LogicalType.ElementType.get();
				if (!ElementType)
				{
					Fail(EArchiveFailureCode::MalformedSerializer, "Logical container child type is missing.");
					return nullptr;
				}
				while (Parent->Elements.size() <= Slot)
				{
					auto Element = std::make_shared<FDefaultDeltaNode>();
					Element->LogicalType = Parent->LogicalType.Kind == ETypeKind::Map
						? ((Parent->Elements.size() % 2 == 0)
							? *Parent->LogicalType.KeyType : *Parent->LogicalType.ValueType)
						: *Parent->LogicalType.ElementType;
					Parent->Elements.push_back(std::move(Element));
				}
				return Parent->Elements[Slot].get();
			}

			auto ResolveAtomicTarget() -> FDefaultDeltaNode*
			{
				FDefaultDeltaNode* Node = CurrentNode();
				if (!Node) return nullptr;
				if (Node->LogicalType.Kind == ETypeKind::Array
					|| Node->LogicalType.Kind == ETypeKind::FixedArray
					|| Node->LogicalType.Kind == ETypeKind::Map)
				{
					auto& State = CaptureStates[Node];
					return MakeElement(Node, State.NextImplicitElement++);
				}
				return Node;
			}

			auto PushContainerElement(uint64 Index, bool bMapValue) -> void
			{
				FDefaultDeltaNode* Parent = CurrentNode();
				if (!Parent)
				{
					Fail(EArchiveFailureCode::MalformedSerializer, "Container path has no active value.");
					return;
				}
				uint64 Slot = Index;
				if (Parent->LogicalType.Kind == ETypeKind::Map) Slot = Index * 2 + (bMapValue ? 1 : 0);
				else if (bMapValue || (Parent->LogicalType.Kind != ETypeKind::Array
					&& Parent->LogicalType.Kind != ETypeKind::FixedArray))
				{
					Fail(EArchiveFailureCode::MalformedSerializer, "Container path kind mismatches its descriptor.");
					return;
				}
				if (FDefaultDeltaNode* Element = MakeElement(Parent, Slot))
				{
					if (Parent->LogicalType.Kind == ETypeKind::Map)
					{
						const auto ParentIt = MapKeyTokens.find(Parent);
						if (ParentIt != MapKeyTokens.end())
						{
							const auto TokenIt = ParentIt->second.find(Index);
							if (TokenIt != ParentIt->second.end()) Element->CanonicalMapKeyToken = TokenIt->second;
						}
					}
					NodeStack.push_back(Element);
				}
			}
		};

		auto SortCapturedNode(FDefaultDeltaNode& Node, FDefaultDeltaDiagnostic& Diagnostic,
			std::string_view Path, uint64& FieldCount, uint32 Depth) -> bool
		{
			if (Depth > DefaultDeltaMaxDepth)
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::DepthLimit;
				Diagnostic.LogicalPath.assign(Path);
				Diagnostic.ObservedValue = Depth;
				Diagnostic.ApplicableLimit = DefaultDeltaMaxDepth;
				return false;
			}
			std::ranges::sort(Node.Fields, FieldLess);
			for (size_t Index = 1; Index < Node.Fields.size(); ++Index)
			{
				if (Node.Fields[Index - 1].Descriptor.DeclaringType == Node.Fields[Index].Descriptor.DeclaringType
					&& Node.Fields[Index - 1].Descriptor.Name == Node.Fields[Index].Descriptor.Name)
				{
					Diagnostic.Reason = EDefaultDeltaFailureReason::DuplicateField;
					Diagnostic.LogicalPath.assign(Path);
					return false;
				}
			}
			for (FDefaultDeltaFieldPlan& Field : Node.Fields)
			{
				if (++FieldCount > DefaultDeltaMaxFields)
				{
					Diagnostic.Reason = EDefaultDeltaFailureReason::FieldLimit;
					Diagnostic.LogicalPath.assign(Path);
					Diagnostic.ObservedValue = FieldCount;
					Diagnostic.ApplicableLimit = DefaultDeltaMaxFields;
					return false;
				}
				const std::string ChildPath = std::format("{}.{}", Path, Field.Descriptor.Name.ToString());
				if (ChildPath.size() > DefaultDeltaMaxPathLength)
				{
					Diagnostic.Reason = EDefaultDeltaFailureReason::PathLimit;
					Diagnostic.LogicalPath.assign(Path);
					Diagnostic.ObservedValue = ChildPath.size();
					Diagnostic.ApplicableLimit = DefaultDeltaMaxPathLength;
					return false;
				}
				if (Field.Value && !SortCapturedNode(*Field.Value, Diagnostic, ChildPath, FieldCount, Depth + 1)) return false;
			}
			for (const auto& Element : Node.Elements)
				if (Element && !SortCapturedNode(*Element, Diagnostic, Path, FieldCount, Depth + 1)) return false;
			return true;
		}

		auto CaptureObject(DObject* Object, EArchivePurpose Purpose,
			std::vector<FDefaultDeltaFieldPlan>& OutFields, FDefaultDeltaDiagnostic& Diagnostic) -> bool
		{
			FLogicalValueCaptureArchive Archive(Purpose);
			{
				auto Scope = Archive.EnterObject(*Object);
				Object->Serialize(Archive);
			}
			if (!Archive.Finish())
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::ArchiveFailure;
				if (const FArchiveFailure* Failure = Archive.GetFailure())
				{
					Diagnostic.ArchiveReason = Failure->Code;
					Diagnostic.LogicalPath = Failure->Path;
				}
				return false;
			}
			OutFields = std::move(Archive.RootFields);
			uint64 FieldCount = 0;
			FDefaultDeltaNode Root;
			Root.LogicalType = FArchiveLogicalTypeDescriptor::Struct(Object->GetClass()->GetQualifiedName());
			Root.Fields = std::move(OutFields);
			if (!SortCapturedNode(Root, Diagnostic, Object->GetName(), FieldCount, 0)) return false;
			OutFields = std::move(Root.Fields);
			return true;
		}

		auto CaptureStructDefault(DStruct* Struct, DObject* ScopeObject,
			std::shared_ptr<FDefaultDeltaNode>& OutValue, FDefaultDeltaDiagnostic& Diagnostic) -> bool
		{
			if (!Struct || !Struct->GetDefaultValue())
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::MissingStructDefault;
				Diagnostic.LogicalPath = Struct ? Struct->GetQualifiedName().ToString() : "<struct>";
				return false;
			}
			FStructProperty RootProperty(
				FFieldVariant(), FName("__DefaultDeltaStruct"), EObjectFlags::NoFlags,
				EPropertyFlags::None, 1, 0, Struct);
			FLogicalValueCaptureArchive Archive(EArchivePurpose::AuthoredPackage);
			{
				auto Scope = Archive.EnterObject(*ScopeObject);
				Archive.MarkBaseReflectedFieldsSerialized();
				SerializeReflectedPropertyValue(
					Archive, RootProperty, const_cast<void*>(Struct->GetDefaultValue()), 0, true);
			}
			if (!Archive.Finish() || Archive.RootFields.size() != 1)
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::ArchiveFailure;
				if (const FArchiveFailure* Failure = Archive.GetFailure())
				{
					Diagnostic.ArchiveReason = Failure->Code;
					Diagnostic.LogicalPath = Failure->Path;
				}
				return false;
			}
			OutValue = std::move(Archive.RootFields.front().Value);
			uint64 FieldCount = 0;
			return SortCapturedNode(*OutValue, Diagnostic, Struct->GetQualifiedName().ToString(), FieldCount, 0);
		}

		auto ManifestEquivalent(const FDefaultDeltaNode& Left, const FDefaultDeltaNode& Right) -> bool;

		auto FieldsManifestEquivalent(
			const std::vector<FDefaultDeltaFieldPlan>& Left,
			const std::vector<FDefaultDeltaFieldPlan>& Right) -> bool
		{
			if (Left.size() != Right.size()) return false;
			for (size_t Index = 0; Index < Left.size(); ++Index)
			{
				if (!DescriptorEquivalent(Left[Index].Descriptor, Right[Index].Descriptor)
					|| !Left[Index].Value || !Right[Index].Value
					|| !ManifestEquivalent(*Left[Index].Value, *Right[Index].Value)) return false;
			}
			return true;
		}

		auto ManifestEquivalent(const FDefaultDeltaNode& Left, const FDefaultDeltaNode& Right) -> bool
		{
			if (!AreArchiveLogicalTypesEquivalent(Left.LogicalType, Right.LogicalType)) return false;
			if (Left.LogicalType.Kind == ETypeKind::Struct)
				return FieldsManifestEquivalent(Left.Fields, Right.Fields);
			return true;
		}

		auto CaptureShapeEquivalent(const FDefaultDeltaNode& Left, const FDefaultDeltaNode& Right) -> bool
		{
			if (!AreArchiveLogicalTypesEquivalent(Left.LogicalType, Right.LogicalType)
				|| Left.CanonicalMapKeyToken != Right.CanonicalMapKeyToken
				|| Left.Fields.size() != Right.Fields.size()
				|| Left.Elements.size() != Right.Elements.size()) return false;
			for (size_t Index = 0; Index < Left.Fields.size(); ++Index)
				if (!DescriptorEquivalent(Left.Fields[Index].Descriptor, Right.Fields[Index].Descriptor)
					|| !Left.Fields[Index].Value || !Right.Fields[Index].Value
					|| !CaptureShapeEquivalent(*Left.Fields[Index].Value, *Right.Fields[Index].Value)) return false;
			for (size_t Index = 0; Index < Left.Elements.size(); ++Index)
				if (!Left.Elements[Index] || !Right.Elements[Index]
					|| !CaptureShapeEquivalent(*Left.Elements[Index], *Right.Elements[Index])) return false;
			return true;
		}

		auto FieldsCaptureShapeEquivalent(
			const std::vector<FDefaultDeltaFieldPlan>& Left,
			const std::vector<FDefaultDeltaFieldPlan>& Right) -> bool
		{
			if (Left.size() != Right.size()) return false;
			for (size_t Index = 0; Index < Left.size(); ++Index)
				if (!DescriptorEquivalent(Left[Index].Descriptor, Right[Index].Descriptor)
					|| !Left[Index].Value || !Right[Index].Value
					|| !CaptureShapeEquivalent(*Left[Index].Value, *Right[Index].Value)) return false;
			return true;
		}

		auto ValidatePathTokens(const FAuthoredOverridePath& Path,
			FAuthoredOverrideDiagnostic& Diagnostic) -> bool
		{
			if (Path.empty())
			{
				Diagnostic.Reason = EAuthoredOverrideFailureReason::EmptyPath;
				return false;
			}
			if (Path.size() > DefaultDeltaMaxDepth)
			{
				Diagnostic.Reason = EAuthoredOverrideFailureReason::DepthLimit;
				return false;
			}
			size_t EncodedLength = 0;
			for (size_t Index = 0; Index < Path.size(); ++Index)
			{
				const auto& Token = Path[Index];
				EncodedLength += Token.DeclaringType.ToString().size()
					+ Token.FieldName.ToString().size() + Token.MapKeyToken.size() + sizeof(Token.Index) + 1;
				const bool bValidField = Token.Kind == EAuthoredOverridePathTokenKind::Field
					&& !Token.DeclaringType.IsNone() && !Token.FieldName.IsNone()
					&& Token.Index == 0 && Token.MapKeyToken.empty();
				const bool bValidIndex = (Token.Kind == EAuthoredOverridePathTokenKind::FixedArrayElement
					|| Token.Kind == EAuthoredOverridePathTokenKind::ArrayElement)
					&& Token.DeclaringType.IsNone() && Token.FieldName.IsNone() && Token.MapKeyToken.empty();
				const bool bValidMap = Token.Kind == EAuthoredOverridePathTokenKind::MapValue
					&& Token.DeclaringType.IsNone() && Token.FieldName.IsNone()
					&& Token.Index == 0 && !Token.MapKeyToken.empty();
				if ((!bValidField && !bValidIndex && !bValidMap) || (Index == 0 && !bValidField))
				{
					Diagnostic.Reason = EAuthoredOverrideFailureReason::InvalidToken;
					return false;
				}
			}
			if (EncodedLength > DefaultDeltaMaxPathLength)
			{
				Diagnostic.Reason = EAuthoredOverrideFailureReason::PathLimit;
				return false;
			}
			return true;
		}

		auto FindPathField(const std::vector<FDefaultDeltaFieldPlan>& Fields,
			const FAuthoredOverridePathToken& Token) -> const FDefaultDeltaFieldPlan*
		{
			const auto It = std::ranges::find_if(Fields, [&](const FDefaultDeltaFieldPlan& Field) {
				return Field.Descriptor.DeclaringType == Token.DeclaringType
					&& Field.Descriptor.Name == Token.FieldName;
			});
			return It == Fields.end() ? nullptr : &*It;
		}

		auto ValidatePathAgainstFields(const std::vector<FDefaultDeltaFieldPlan>& Fields,
			const FAuthoredOverridePath& Path, FAuthoredOverrideDiagnostic& Diagnostic) -> bool
		{
			const FDefaultDeltaFieldPlan* Field = FindPathField(Fields, Path.front());
			if (!Field || !Field->Value)
			{
				Diagnostic.Reason = EAuthoredOverrideFailureReason::FieldNotFound;
				return false;
			}
			const FDefaultDeltaNode* Node = Field->Value.get();
			for (size_t Index = 1; Index < Path.size(); ++Index)
			{
				const auto& Token = Path[Index];
				switch (Token.Kind)
				{
				case EAuthoredOverridePathTokenKind::Field:
					if (Node->LogicalType.Kind != ETypeKind::Struct)
					{
						Diagnostic.Reason = EAuthoredOverrideFailureReason::TypeMismatch;
						return false;
					}
					Field = FindPathField(Node->Fields, Token);
					if (!Field || !Field->Value)
					{
						Diagnostic.Reason = EAuthoredOverrideFailureReason::FieldNotFound;
						return false;
					}
					Node = Field->Value.get();
					break;
				case EAuthoredOverridePathTokenKind::FixedArrayElement:
				case EAuthoredOverridePathTokenKind::ArrayElement:
					if ((Token.Kind == EAuthoredOverridePathTokenKind::FixedArrayElement
							&& Node->LogicalType.Kind != ETypeKind::FixedArray)
						|| (Token.Kind == EAuthoredOverridePathTokenKind::ArrayElement
							&& Node->LogicalType.Kind != ETypeKind::Array))
					{
						Diagnostic.Reason = EAuthoredOverrideFailureReason::TypeMismatch;
						return false;
					}
					if (Token.Index >= Node->Elements.size() || !Node->Elements[Token.Index])
					{
						Diagnostic.Reason = EAuthoredOverrideFailureReason::IndexOutOfRange;
						return false;
					}
					Node = Node->Elements[Token.Index].get();
					break;
				case EAuthoredOverridePathTokenKind::MapValue:
					if (Node->LogicalType.Kind != ETypeKind::Map)
					{
						Diagnostic.Reason = EAuthoredOverrideFailureReason::TypeMismatch;
						return false;
					}
					{
						const auto It = std::ranges::find_if(Node->Elements, [&](const auto& Element) {
							return Element && Element->CanonicalMapKeyToken == Token.MapKeyToken;
						});
						if (It == Node->Elements.end())
						{
							const bool bAnyToken = std::ranges::any_of(Node->Elements,
								[](const auto& Element) { return Element && !Element->CanonicalMapKeyToken.empty(); });
							Diagnostic.Reason = bAnyToken ? EAuthoredOverrideFailureReason::MapKeyNotFound
								: EAuthoredOverrideFailureReason::MapKeyUnavailable;
							return false;
						}
						const size_t KeyIndex = static_cast<size_t>(std::distance(Node->Elements.begin(), It));
						if ((KeyIndex % 2) != 0 || KeyIndex + 1 >= Node->Elements.size()
							|| !Node->Elements[KeyIndex + 1])
						{
							Diagnostic.Reason = EAuthoredOverrideFailureReason::SchemaMismatch;
							return false;
						}
						Node = Node->Elements[KeyIndex + 1].get();
					}
					break;
				}
			}
			return true;
		}

		struct FPlannerContext
		{
			FDefaultDeltaPlan& Plan;
			FDefaultDeltaDiagnostic& Diagnostic;
			const FDefaultObjectGraphMap* Graph = nullptr;
			DObject* ScopeObject = nullptr;
			std::span<const FAuthoredOverrideEntry> LedgerEntries;
		};

		auto FindAuthoredIntent(std::span<const FAuthoredOverrideEntry> Entries,
			const FAuthoredOverridePath& Path) -> std::optional<EAuthoredOverrideProvenance>
		{
			std::optional<EAuthoredOverrideProvenance> Result;
			for (const FAuthoredOverrideEntry& Entry : Entries)
			{
				if (!IsAuthoredOverridePathPrefix(Path, Entry.Path)) continue;
				if (Entry.Provenance == EAuthoredOverrideProvenance::Forced) return Entry.Provenance;
				Result = Entry.Provenance;
			}
			return Result;
		}

		auto ToDeltaProvenance(std::optional<EAuthoredOverrideProvenance> Intent)
			-> EDefaultDeltaProvenance
		{
			return Intent == EAuthoredOverrideProvenance::Forced
				? EDefaultDeltaProvenance::Forced
				: (Intent ? EDefaultDeltaProvenance::Explicit : EDefaultDeltaProvenance::None);
		}

		auto AppendContainerPath(const FDefaultDeltaNode& Parent, size_t ElementIndex,
			FAuthoredOverridePath& Path) -> bool
		{
			if (Parent.LogicalType.Kind == ETypeKind::FixedArray)
				Path.push_back(FAuthoredOverridePathToken::FixedArrayElement(ElementIndex));
			else if (Parent.LogicalType.Kind == ETypeKind::Array)
				Path.push_back(FAuthoredOverridePathToken::ArrayElement(ElementIndex));
			else if (Parent.LogicalType.Kind == ETypeKind::Map)
			{
				if (ElementIndex % 2 == 0) return false;
				const auto& Key = Parent.Elements[ElementIndex - 1];
				if (!Key || Key->CanonicalMapKeyToken.empty()) return false;
				Path.push_back(FAuthoredOverridePathToken::MapValue(Key->CanonicalMapKeyToken));
			}
			else return false;
			return true;
		}

		auto CompareNodes(const FDefaultDeltaNode& Left, const FDefaultDeltaNode& Right,
			FPlannerContext& Context, uint32 Depth) -> EPropertyIdentityResult
		{
			++Context.Plan.ComparisonCount;
			Context.Plan.MaximumDepth = std::max(Context.Plan.MaximumDepth, Depth);
			if (Depth > DefaultDeltaMaxDepth || !AreArchiveLogicalTypesEquivalent(Left.LogicalType, Right.LogicalType))
				return EPropertyIdentityResult::Unsupported;
			switch (Left.LogicalType.Kind)
			{
			case ETypeKind::Scalar:
				if (!Left.bHasAtomicValue || !Right.bHasAtomicValue) return EPropertyIdentityResult::Unsupported;
				if (Left.LogicalType.bFloating) return Left.FloatingBits == Right.FloatingBits
					? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
				if (Left.LogicalType.bSigned) return Left.SignedValue == Right.SignedValue
					? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
				return Left.UnsignedValue == Right.UnsignedValue
					? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
			case ETypeKind::Enum:
				if (!Left.bHasAtomicValue || !Right.bHasAtomicValue) return EPropertyIdentityResult::Unsupported;
				return (Left.LogicalType.bSigned ? Left.SignedValue == Right.SignedValue
					: Left.UnsignedValue == Right.UnsignedValue)
					? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
			case ETypeKind::String:
			case ETypeKind::Name:
			case ETypeKind::SoftObject:
				return Left.bHasAtomicValue && Right.bHasAtomicValue
					? (Left.TextValue == Right.TextValue ? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different)
					: EPropertyIdentityResult::Unsupported;
			case ETypeKind::Guid:
				return Left.bHasAtomicValue && Right.bHasAtomicValue
					? (Left.GuidValue == Right.GuidValue ? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different)
					: EPropertyIdentityResult::Unsupported;
			case ETypeKind::Bytes:
			case ETypeKind::BulkData:
				return Left.bHasAtomicValue && Right.bHasAtomicValue
					? (Left.ByteValue == Right.ByteValue ? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different)
					: EPropertyIdentityResult::Unsupported;
			case ETypeKind::Object:
				if (!Left.bHasAtomicValue || !Right.bHasAtomicValue) return EPropertyIdentityResult::Unsupported;
				return (Left.ObjectValue == Right.ObjectValue
					|| (Context.Graph && Context.Graph->AreReferencesEquivalent(Left.ObjectValue, Right.ObjectValue)))
					? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
			case ETypeKind::WeakObject:
				return EPropertyIdentityResult::Unsupported;
			case ETypeKind::Struct:
				if (Left.SourceStruct && Left.SourceStruct == Right.SourceStruct
					&& Left.SourceStruct->HasIdentical())
				{
					if (!Left.SourceValue || !Right.SourceValue) return EPropertyIdentityResult::Unsupported;
					return Left.SourceStruct->GetOps().Identical(Left.SourceValue, Right.SourceValue)
						? EPropertyIdentityResult::Identical : EPropertyIdentityResult::Different;
				}
				if (!FieldsManifestEquivalent(Left.Fields, Right.Fields)) return EPropertyIdentityResult::Unsupported;
				for (size_t Index = 0; Index < Left.Fields.size(); ++Index)
				{
					const auto Result = CompareNodes(*Left.Fields[Index].Value, *Right.Fields[Index].Value, Context, Depth + 1);
					if (Result != EPropertyIdentityResult::Identical) return Result;
				}
				return EPropertyIdentityResult::Identical;
			case ETypeKind::Array:
			case ETypeKind::Map:
			case ETypeKind::FixedArray:
				if (Left.Elements.size() != Right.Elements.size()) return EPropertyIdentityResult::Different;
				for (size_t Index = 0; Index < Left.Elements.size(); ++Index)
				{
					const auto Result = CompareNodes(*Left.Elements[Index], *Right.Elements[Index], Context, Depth + 1);
					if (Result != EPropertyIdentityResult::Identical) return Result;
				}
				return EPropertyIdentityResult::Identical;
			}
			return EPropertyIdentityResult::Unsupported;
		}

		auto CloneNode(const FDefaultDeltaNode& Source) -> std::shared_ptr<FDefaultDeltaNode>
		{
			auto Result = std::make_shared<FDefaultDeltaNode>(Source);
			Result->SourceValue = nullptr;
			Result->SourceStruct = nullptr;
			Result->Fields.clear();
			Result->Elements.clear();
			for (const FDefaultDeltaFieldPlan& Field : Source.Fields)
			{
				FDefaultDeltaFieldPlan Copy = Field;
				Copy.Value = Field.Value ? CloneNode(*Field.Value) : nullptr;
				Result->Fields.push_back(std::move(Copy));
			}
			for (const auto& Element : Source.Elements)
				Result->Elements.push_back(Element ? CloneNode(*Element) : nullptr);
			return Result;
		}

		auto BuildForcedValue(const FDefaultDeltaNode& Source, FDefaultDeltaPlan& Plan,
			FDefaultDeltaDiagnostic& Diagnostic, uint32 Depth) -> std::shared_ptr<FDefaultDeltaNode>
		{
			if (Depth > DefaultDeltaMaxDepth)
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::DepthLimit;
				return nullptr;
			}
			if (Source.LogicalType.Kind == ETypeKind::Struct && Source.SourceStruct
				&& (!Source.SourceStruct->HasCompleteAuthoredFields() || Source.SourceStruct->HasSerializer()))
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::UnsupportedLogicalType;
				Diagnostic.LogicalPath = Source.LogicalType.QualifiedType.ToString();
				return nullptr;
			}
			Plan.MaximumDepth = std::max(Plan.MaximumDepth, Depth);
			auto Result = CloneNode(Source);
			Result->Baseline = EDefaultDeltaBaselineKind::None;
			Result->Disposition = EDefaultDeltaDisposition::Emitted;
			Result->Provenance = EDefaultDeltaProvenance::Forced;
			Result->Identity = EPropertyIdentityResult::Different;
			Result->Fields.clear();
			for (const FDefaultDeltaFieldPlan& SourceField : Source.Fields)
			{
				FDefaultDeltaFieldPlan Field = SourceField;
				Field.Baseline = EDefaultDeltaBaselineKind::None;
				Field.Disposition = EDefaultDeltaDisposition::Emitted;
				Field.Provenance = EDefaultDeltaProvenance::Forced;
				Field.Identity = EPropertyIdentityResult::Different;
				Field.Value = SourceField.Value
					? BuildForcedValue(*SourceField.Value, Plan, Diagnostic, Depth + 1) : nullptr;
				if (!Field.Value) return nullptr;
				++Plan.FieldCount;
				++Plan.EmittedFieldCount;
				Result->Fields.push_back(std::move(Field));
			}
			Result->Elements.clear();
			for (const auto& SourceElement : Source.Elements)
			{
				auto Element = SourceElement
					? BuildForcedValue(*SourceElement, Plan, Diagnostic, Depth + 1) : nullptr;
				if (!Element) return nullptr;
				Result->Elements.push_back(std::move(Element));
			}
			return Result;
		}

		auto BuildPlannedValue(const FDefaultDeltaNode& Live, FPlannerContext& Context,
			uint32 Depth, bool bUseStructDefaults, const FAuthoredOverridePath& Path)
			-> std::shared_ptr<FDefaultDeltaNode>
		{
			if (Depth > DefaultDeltaMaxDepth)
			{
				Context.Diagnostic.Reason = EDefaultDeltaFailureReason::DepthLimit;
				return nullptr;
			}
			auto Planned = CloneNode(Live);
			const auto NodeIntent = FindAuthoredIntent(Context.LedgerEntries, Path);
			Planned->Baseline = EDefaultDeltaBaselineKind::None;
			Planned->Disposition = EDefaultDeltaDisposition::Emitted;
			Planned->Provenance = NodeIntent
				? ToDeltaProvenance(NodeIntent) : EDefaultDeltaProvenance::Explicit;
			Planned->Identity = EPropertyIdentityResult::Different;
			if (Live.LogicalType.Kind == ETypeKind::Struct)
			{
				Planned->Fields.clear();
				std::shared_ptr<FDefaultDeltaNode> Default;
				if (bUseStructDefaults)
				{
					DStruct* Struct = FindStructByQualifiedName(Live.LogicalType.QualifiedType);
					if (!CaptureStructDefault(Struct, Context.ScopeObject, Default, Context.Diagnostic)) return nullptr;
					if (!FieldsManifestEquivalent(Live.Fields, Default->Fields))
					{
						Context.Diagnostic.Reason = EDefaultDeltaFailureReason::ManifestMismatch;
						Context.Diagnostic.LogicalPath = Live.LogicalType.QualifiedType.ToString();
						return nullptr;
					}
				}
				for (size_t Index = 0; Index < Live.Fields.size(); ++Index)
				{
					FDefaultDeltaFieldPlan Field = Live.Fields[Index];
					FAuthoredOverridePath ChildPath = Path;
					ChildPath.push_back(FAuthoredOverridePathToken::Field(
						Field.Descriptor.DeclaringType, Field.Descriptor.Name));
					const auto Intent = FindAuthoredIntent(Context.LedgerEntries, ChildPath);
					Field.Baseline = bUseStructDefaults
						? EDefaultDeltaBaselineKind::StructTypeDefault : EDefaultDeltaBaselineKind::None;
					Field.Identity = bUseStructDefaults
						? CompareNodes(*Live.Fields[Index].Value, *Default->Fields[Index].Value, Context, Depth + 1)
						: EPropertyIdentityResult::Different;
					if (Field.Identity == EPropertyIdentityResult::Unsupported)
					{
						Context.Diagnostic.Reason = EDefaultDeltaFailureReason::UnsupportedIdentity;
						return nullptr;
					}
					Field.Disposition = Field.Identity == EPropertyIdentityResult::Identical && !Intent
						? EDefaultDeltaDisposition::Omitted : EDefaultDeltaDisposition::Emitted;
					Field.Provenance = Intent ? ToDeltaProvenance(Intent)
						: (Field.Disposition == EDefaultDeltaDisposition::Emitted
							? EDefaultDeltaProvenance::Explicit : EDefaultDeltaProvenance::None);
					Field.Value = Field.Disposition == EDefaultDeltaDisposition::Emitted
						? BuildPlannedValue(*Live.Fields[Index].Value, Context, Depth + 1,
							bUseStructDefaults, ChildPath)
						: nullptr;
					if (Field.Disposition == EDefaultDeltaDisposition::Emitted && !Field.Value) return nullptr;
					++Context.Plan.FieldCount;
					if (Field.Disposition == EDefaultDeltaDisposition::Emitted) ++Context.Plan.EmittedFieldCount;
					else ++Context.Plan.OmittedFieldCount;
					Planned->Fields.push_back(std::move(Field));
				}
			}
			else if (Live.LogicalType.Kind == ETypeKind::Array
				|| Live.LogicalType.Kind == ETypeKind::Map
				|| Live.LogicalType.Kind == ETypeKind::FixedArray)
			{
				Planned->Elements.clear();
				for (size_t Index = 0; Index < Live.Elements.size(); ++Index)
				{
					const bool bMapKey = Live.LogicalType.Kind == ETypeKind::Map && Index % 2 == 0;
					FAuthoredOverridePath ChildPath = Path;
					if (!bMapKey && !AppendContainerPath(Live, Index, ChildPath))
						ChildPath.push_back(FAuthoredOverridePathToken::MapValue({}));
					auto Element = bMapKey ? CloneNode(*Live.Elements[Index])
						: BuildPlannedValue(*Live.Elements[Index], Context, Depth + 1,
							bUseStructDefaults, ChildPath);
					if (!Element) return nullptr;
					Planned->Elements.push_back(std::move(Element));
				}
			}
			return Planned;
		}

		auto PlanObjectPair(DObject* Live, const DObject* Default, const FDefaultObjectGraphMap& Graph,
			FDefaultDeltaPlan& Plan, FDefaultDeltaDiagnostic& Diagnostic) -> bool
		{
			std::vector<FDefaultDeltaFieldPlan> LiveDiscovery, DefaultDiscovery, LiveValues, DefaultValues;
			if (!CaptureObject(Live, EArchivePurpose::Discovery, LiveDiscovery, Diagnostic)
				|| !CaptureObject(const_cast<DObject*>(Default), EArchivePurpose::Discovery, DefaultDiscovery, Diagnostic)
				|| !CaptureObject(Live, EArchivePurpose::AuthoredPackage, LiveValues, Diagnostic)
				|| !CaptureObject(const_cast<DObject*>(Default), EArchivePurpose::AuthoredPackage, DefaultValues, Diagnostic))
				return false;
			if (!FieldsCaptureShapeEquivalent(LiveDiscovery, LiveValues)
				|| !FieldsCaptureShapeEquivalent(DefaultDiscovery, DefaultValues)
				|| !FieldsManifestEquivalent(LiveValues, DefaultValues))
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::ManifestMismatch;
				Diagnostic.LogicalPath = Live->GetName();
				return false;
			}
			std::vector<FAuthoredOverrideEntry> ValidLedgerEntries;
			for (const FAuthoredOverrideEntry& Entry : Live->GetAuthoredOverrideEntries())
			{
				FAuthoredOverrideDiagnostic LedgerDiagnostic;
				if (ValidatePathTokens(Entry.Path, LedgerDiagnostic)
					&& ValidatePathAgainstFields(LiveValues, Entry.Path, LedgerDiagnostic))
				{
					ValidLedgerEntries.push_back(Entry);
					continue;
				}
				if (LedgerDiagnostic.Reason == EAuthoredOverrideFailureReason::FieldNotFound
					|| LedgerDiagnostic.Reason == EAuthoredOverrideFailureReason::IndexOutOfRange
					|| LedgerDiagnostic.Reason == EAuthoredOverrideFailureReason::MapKeyNotFound)
					continue;
				Diagnostic.Reason = EDefaultDeltaFailureReason::AuthoredOverrideFailure;
				Diagnostic.AuthoredOverrideReason = LedgerDiagnostic.Reason;
				Diagnostic.LogicalPath = LedgerDiagnostic.LogicalPath;
				return false;
			}
			FDefaultDeltaObjectPlan ObjectPlan{.Object = Live, .ClassDefaultObject = Default};
			FPlannerContext Context{Plan, Diagnostic, &Graph, Live, ValidLedgerEntries};
			for (size_t Index = 0; Index < LiveValues.size(); ++Index)
			{
				FDefaultDeltaFieldPlan Field = LiveValues[Index];
				const FAuthoredOverridePath Path{FAuthoredOverridePathToken::Field(
					Field.Descriptor.DeclaringType, Field.Descriptor.Name)};
				const auto Intent = FindAuthoredIntent(Context.LedgerEntries, Path);
				Field.Baseline = EDefaultDeltaBaselineKind::ClassDefault;
				Field.Identity = CompareNodes(*LiveValues[Index].Value, *DefaultValues[Index].Value, Context, 1);
				if (Field.Identity == EPropertyIdentityResult::Unsupported)
				{
					Diagnostic.Reason = EDefaultDeltaFailureReason::UnsupportedIdentity;
					Diagnostic.LogicalPath = Field.Descriptor.Name.ToString();
					return false;
				}
				Field.Disposition = Field.Identity == EPropertyIdentityResult::Identical && !Intent
					? EDefaultDeltaDisposition::Omitted : EDefaultDeltaDisposition::Emitted;
				Field.Provenance = Intent ? ToDeltaProvenance(Intent)
					: (Field.Disposition == EDefaultDeltaDisposition::Emitted
						? EDefaultDeltaProvenance::Explicit : EDefaultDeltaProvenance::None);
				Field.Value = Field.Disposition == EDefaultDeltaDisposition::Emitted
					? BuildPlannedValue(*LiveValues[Index].Value, Context, 1, true, Path) : nullptr;
				if (Field.Disposition == EDefaultDeltaDisposition::Emitted && !Field.Value) return false;
				++Plan.FieldCount;
				if (Field.Disposition == EDefaultDeltaDisposition::Emitted) ++Plan.EmittedFieldCount;
				else ++Plan.OmittedFieldCount;
				ObjectPlan.Fields.push_back(std::move(Field));
			}
			Plan.Objects.push_back(std::move(ObjectPlan));
			return true;
		}
	}

	auto AreArchiveLogicalTypesEquivalent(
		const FArchiveLogicalTypeDescriptor& Left,
		const FArchiveLogicalTypeDescriptor& Right) -> bool
	{
		if (Left.Kind != Right.Kind || Left.bSigned != Right.bSigned
			|| Left.bFloating != Right.bFloating || Left.BitWidth != Right.BitWidth
			|| Left.QualifiedType != Right.QualifiedType
			|| Left.NativeFieldVersion != Right.NativeFieldVersion
			|| Left.FixedArrayDimension != Right.FixedArrayDimension) return false;
		auto ChildEqual = [](const auto& A, const auto& B) {
			return (!A && !B) || (A && B && AreArchiveLogicalTypesEquivalent(*A, *B));
		};
		return ChildEqual(Left.ElementType, Right.ElementType)
			&& ChildEqual(Left.KeyType, Right.KeyType)
			&& ChildEqual(Left.ValueType, Right.ValueType);
	}

	auto ValidateAuthoredOverridePath(DObject* Object, const FAuthoredOverridePath& Path,
		FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		const FAuthoredOverrideEntry Entry{Path, EAuthoredOverrideProvenance::LoadedExplicit};
		return ValidateAuthoredOverrideEntries(Object, std::span(&Entry, 1), OutDiagnostic);
	}

	auto ValidateAuthoredOverrideEntries(DObject* Object,
		std::span<const FAuthoredOverrideEntry> Entries,
		FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		FAuthoredOverrideDiagnostic Diagnostic;
		auto Fail = [&]() {
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return false;
		};
		if (!Object || !Object->GetClass())
		{
			Diagnostic.Reason = EAuthoredOverrideFailureReason::InvalidObject;
			return Fail();
		}
		if (Object->IsTemplateObject())
		{
			Diagnostic.Reason = EAuthoredOverrideFailureReason::TemplateObject;
			return Fail();
		}
		for (const FAuthoredOverrideEntry& Entry : Entries)
			if (!ValidatePathTokens(Entry.Path, Diagnostic)) return Fail();
		FDefaultDeltaDiagnostic CaptureDiagnostic;
		std::vector<FDefaultDeltaFieldPlan> Discovery, Values;
		if (!CaptureObject(Object, EArchivePurpose::Discovery, Discovery, CaptureDiagnostic)
			|| !CaptureObject(Object, EArchivePurpose::AuthoredPackage, Values, CaptureDiagnostic))
		{
			Diagnostic.Reason = EAuthoredOverrideFailureReason::ArchiveFailure;
			Diagnostic.LogicalPath = CaptureDiagnostic.LogicalPath;
			return Fail();
		}
		if (!FieldsCaptureShapeEquivalent(Discovery, Values))
		{
			Diagnostic.Reason = EAuthoredOverrideFailureReason::SchemaMismatch;
			return Fail();
		}
		for (const FAuthoredOverrideEntry& Entry : Entries)
			if (!ValidatePathAgainstFields(Values, Entry.Path, Diagnostic)) return Fail();
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto BuildDefaultDeltaPlan(DObject* RootObject, EDefaultDeltaMode Mode,
		FDefaultDeltaPlan& OutPlan, FDefaultDeltaDiagnostic* OutDiagnostic) -> bool
	{
		OutPlan.Reset();
		OutPlan.Mode = Mode;
		FDefaultDeltaDiagnostic Diagnostic;
		auto Fail = [&]() {
			OutPlan.Reset();
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return false;
		};
		if (!RootObject || RootObject->IsTemplateObject() || !RootObject->GetClass())
		{
			Diagnostic.Reason = EDefaultDeltaFailureReason::InvalidInput;
			Diagnostic.LogicalPath = "<root>";
			return Fail();
		}
		if (Mode == EDefaultDeltaMode::NoDelta)
		{
			std::vector<DObject*> Objects{RootObject};
			for (size_t Index = 0; Index < Objects.size(); ++Index)
				for (DObject* Child : GDObjectArray.GetObjectsWithOuter(
					Objects[Index], EObjectQueryScope::LiveOnly, false)) Objects.push_back(Child);
			if (Objects.size() > DefaultDeltaMaxFields)
			{
				Diagnostic.Reason = EDefaultDeltaFailureReason::FieldLimit;
				return Fail();
			}
			std::ranges::sort(Objects, [](const DObject* Left, const DObject* Right) {
				return Left->GetObjectPath() < Right->GetObjectPath();
			});
			for (DObject* Object : Objects)
			{
				std::vector<FDefaultDeltaFieldPlan> Discovery, Values;
				if (!CaptureObject(Object, EArchivePurpose::Discovery, Discovery, Diagnostic)
					|| !CaptureObject(Object, EArchivePurpose::AuthoredPackage, Values, Diagnostic)) return Fail();
				if (!FieldsCaptureShapeEquivalent(Discovery, Values))
				{
					Diagnostic.Reason = EDefaultDeltaFailureReason::ManifestMismatch;
					return Fail();
				}
				FDefaultDeltaObjectPlan ObjectPlan{.Object = Object};
				for (FDefaultDeltaFieldPlan& Field : Values)
				{
					Field.Value = Field.Value
						? BuildForcedValue(*Field.Value, OutPlan, Diagnostic, 1) : nullptr;
					if (!Field.Value) return Fail();
					Field.Baseline = EDefaultDeltaBaselineKind::None;
					Field.Disposition = EDefaultDeltaDisposition::Emitted;
					Field.Provenance = EDefaultDeltaProvenance::Forced;
					Field.Identity = EPropertyIdentityResult::Different;
					++OutPlan.FieldCount;
					++OutPlan.EmittedFieldCount;
					ObjectPlan.Fields.push_back(std::move(Field));
				}
				OutPlan.Objects.push_back(std::move(ObjectPlan));
			}
			if (OutDiagnostic) OutDiagnostic->Reset();
			return true;
		}

		const DObject* DefaultObject = RootObject->GetClass()->GetDefaultObject();
		if (!DefaultObject)
		{
			Diagnostic.Reason = EDefaultDeltaFailureReason::MissingClassDefault;
			Diagnostic.LogicalPath = RootObject->GetClass()->GetQualifiedName().ToString();
			return Fail();
		}
		FDefaultObjectGraphMap Graph;
		FDefaultObjectGraphDiagnostic GraphDiagnostic;
		if (!Graph.Build(DefaultObject, RootObject, &GraphDiagnostic))
		{
			Diagnostic.Reason = EDefaultDeltaFailureReason::DefaultObjectGraphFailure;
			Diagnostic.GraphReason = GraphDiagnostic.Reason;
			Diagnostic.LogicalPath = GraphDiagnostic.LogicalPath;
			return Fail();
		}
		std::vector<const DObject*> Templates{DefaultObject};
		for (size_t Index = 0; Index < Templates.size(); ++Index)
		{
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(
				Templates[Index], EObjectQueryScope::IncludeTemplates, false))
				Templates.push_back(Child);
		}
		std::ranges::sort(Templates, [](const DObject* Left, const DObject* Right) {
			return Left->GetObjectPath() < Right->GetObjectPath();
		});
		for (const DObject* Template : Templates)
		{
			DObject* Live = const_cast<DObject*>(Graph.FindInstance(Template));
			if (!Live || !PlanObjectPair(Live, Template, Graph, OutPlan, Diagnostic)) return Fail();
		}
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto AreDefaultDeltaPlansEquivalent(
		const FDefaultDeltaPlan& Left,
		const FDefaultDeltaPlan& Right) -> bool
	{
		std::function<bool(const FDefaultDeltaNode&, const FDefaultDeltaNode&)> NodesEqual;
		std::function<bool(const FDefaultDeltaFieldPlan&, const FDefaultDeltaFieldPlan&)> FieldsEqual;
		FieldsEqual = [&](const FDefaultDeltaFieldPlan& A, const FDefaultDeltaFieldPlan& B) {
			return DescriptorEquivalent(A.Descriptor, B.Descriptor)
				&& A.Baseline == B.Baseline && A.Disposition == B.Disposition
				&& A.Provenance == B.Provenance && A.Identity == B.Identity
				&& ((!A.Value && !B.Value) || (A.Value && B.Value && NodesEqual(*A.Value, *B.Value)));
		};
		NodesEqual = [&](const FDefaultDeltaNode& A, const FDefaultDeltaNode& B) {
			if (!AreArchiveLogicalTypesEquivalent(A.LogicalType, B.LogicalType)
				|| A.Baseline != B.Baseline || A.Disposition != B.Disposition
				|| A.Provenance != B.Provenance || A.Identity != B.Identity
				|| A.BoolValue != B.BoolValue || A.SignedValue != B.SignedValue
				|| A.UnsignedValue != B.UnsignedValue || A.FloatingBits != B.FloatingBits
				|| A.TextValue != B.TextValue || A.GuidValue != B.GuidValue
				|| A.ByteValue != B.ByteValue || A.CanonicalMapKeyToken != B.CanonicalMapKeyToken
				|| A.ObjectValue != B.ObjectValue
				|| A.bHasAtomicValue != B.bHasAtomicValue
				|| A.Fields.size() != B.Fields.size() || A.Elements.size() != B.Elements.size()) return false;
			for (size_t Index = 0; Index < A.Fields.size(); ++Index)
				if (!FieldsEqual(A.Fields[Index], B.Fields[Index])) return false;
			for (size_t Index = 0; Index < A.Elements.size(); ++Index)
				if ((!A.Elements[Index] != !B.Elements[Index])
					|| (A.Elements[Index] && !NodesEqual(*A.Elements[Index], *B.Elements[Index]))) return false;
			return true;
		};
		if (Left.Mode != Right.Mode || Left.FieldCount != Right.FieldCount
			|| Left.EmittedFieldCount != Right.EmittedFieldCount
			|| Left.OmittedFieldCount != Right.OmittedFieldCount
			|| Left.ComparisonCount != Right.ComparisonCount
			|| Left.MaximumDepth != Right.MaximumDepth
			|| Left.Objects.size() != Right.Objects.size()) return false;
		for (size_t ObjectIndex = 0; ObjectIndex < Left.Objects.size(); ++ObjectIndex)
		{
			const auto& A = Left.Objects[ObjectIndex];
			const auto& B = Right.Objects[ObjectIndex];
			if (A.Object != B.Object || A.ClassDefaultObject != B.ClassDefaultObject
				|| A.Fields.size() != B.Fields.size()) return false;
			for (size_t FieldIndex = 0; FieldIndex < A.Fields.size(); ++FieldIndex)
				if (!FieldsEqual(A.Fields[FieldIndex], B.Fields[FieldIndex])) return false;
		}
		return true;
	}
}
