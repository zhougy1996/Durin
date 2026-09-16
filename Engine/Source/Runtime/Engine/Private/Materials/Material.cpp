#include "Materials/Material.h"
#include "Materials/MaterialCustomVersion.h"
#include "Logging/LogMacros.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderTypes.h"
#include "Asset/Asset.h"
#include "DObject/Property.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Modules/ModuleManager.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		template <typename TValue, typename TReadValue>
		auto GetTypedParameterValue(
			const DMaterial& Material, FName Name, EMaterialParameterType Type,
			TValue& OutValue, TReadValue ReadValue) -> bool
		{
			const auto* Definition = Material.FindParameterDefinition(Name);
			if (!Definition || Definition->Type != Type) return false;
			OutValue = ReadValue(Definition->Value);
			return true;
		}

		auto AdvanceRevision(uint64& Revision) -> void
		{
			Revision = Revision == std::numeric_limits<uint64>::max()
				? 1 : Revision + 1;
		}

	}

	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose)
			&& ObjectInitializer.Purpose != EObjectConstructionPurpose::AssetLoad
			&& ObjectInitializer.Purpose != EObjectConstructionPurpose::Duplication)
		{
			auto* Output = NewObject<DMaterialExpressionMaterialOutput>(this, "MaterialOutput");
			Output->Id = FGuid::NewGuid();
			ExpressionCollection.Expressions.push_back(Output);
		}
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose))
		{
			ValidateExpressionGraph(ExpressionCollection, GetExpressionOutputs(), &ObservedExpressionCode);
			if (!IsMaterialCompilationAcceptingRequests())
				RequestProgramCompile(StaticProperties);
			PublishMaterialRenderProxyState();
		}
	}

	auto DMaterial::AdvanceAuthoredRevision() -> void
	{
		AdvanceRevision(CompilationOwner.MaterialCompileStatus.AuthoredRevision);
		InvalidateMaterialCompilation(false);
	}

	auto DMaterial::SetEditCompileMode(EMaterialEditCompileMode Mode) -> void
	{
		if (EditCompileMode == Mode) return;
		EditCompileMode = Mode;
		for (const auto Handle : GetLoadedMaterialDependents(this))
			if (auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle)); IsValid(Owner))
			{
				const auto State = Owner->GetMaterialCompileStatus().State;
				if (State == EMaterialCompileState::NeedsCompile || State == EMaterialCompileState::Scheduled)
					Private::FMaterialCompilationLifecycle::ScheduleEdit(*Owner);
			}
	}

	auto DMaterial::CompileEdits() -> bool
	{
		bool bAccepted = RequestMaterialRecompile(*this);
		for (const auto Handle : GetLoadedMaterialDependents(this))
			if (auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle));
				IsValid(Owner) && Owner != this)
				bAccepted = RequestMaterialRecompile(*Owner) && bAccepted;
		return bAccepted;
	}



	auto DMaterial::SetMaterialGraphPresentation(
		FMaterialGraphPresentation InPresentation) -> EMaterialGraphPresentationResult
	{
		std::vector<FGuid> Ids;
		for (const auto& Expression : ExpressionCollection.Expressions) if (Expression) Ids.push_back(Expression->Id);
		InPresentation = SanitizeMaterialGraphPresentation(InPresentation, Ids);
		if (GraphPresentation == InPresentation) return EMaterialGraphPresentationResult::NoChange;
		GraphPresentation = std::move(InPresentation);
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return EMaterialGraphPresentationResult::Changed;
	}

	auto DMaterial::ApplyMaterialGraphNodePositions(
		std::span<const FMaterialGraphNodePresentation> Positions,
		uint64 ExpectedAuthoredRevision) -> EMaterialGraphPresentationResult
	{
		if (CompilationOwner.MaterialCompileStatus.AuthoredRevision != ExpectedAuthoredRevision
			|| Positions.size() > MaterialProgramMaxNodeCount)
			return EMaterialGraphPresentationResult::Rejected;
		std::unordered_set<FGuid> RequestedNodes;
		RequestedNodes.reserve(Positions.size());
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (!Position.NodeId.IsValid()
				|| !RequestedNodes.insert(Position.NodeId).second
				|| Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit
				|| !std::ranges::any_of(ExpressionCollection.Expressions,
					[&](const auto& Expression) { return Expression && Expression->Id == Position.NodeId; }))
				return EMaterialGraphPresentationResult::Rejected;
		}

		bool bChanged = false;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			auto It = std::ranges::lower_bound(
				GraphPresentation.Nodes, Position.NodeId, {},
				&FMaterialGraphNodePresentation::NodeId);
			if (It == GraphPresentation.Nodes.end() || It->NodeId != Position.NodeId)
			{
				GraphPresentation.Nodes.insert(It, Position);
				bChanged = true;
			}
			else if (It->X != Position.X || It->Y != Position.Y)
			{
				It->X = Position.X; It->Y = Position.Y;
				bChanged = true;
			}
		}
		if (!bChanged) return EMaterialGraphPresentationResult::NoChange;
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return EMaterialGraphPresentationResult::Changed;
	}

	auto DMaterial::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return ParameterSchema;
	}

	auto DMaterial::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		OutParameter.Definition = Definition;
		OutParameter.Value = Definition->Value;
		OutParameter.Source = const_cast<DMaterial*>(this);
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterial::SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool
	{
		std::string Error;
		if (!ValidateMaterialStaticProperties(InProperties, Error)) return false;
		if (StaticProperties == InProperties) return true;
		const bool bShaderIdentityChanged =
			CanonicalizeMaterialShaderProperties(StaticProperties)
				!= CanonicalizeMaterialShaderProperties(InProperties);
		StaticProperties = InProperties;
		if (bShaderIdentityChanged)
		{
			AdvanceRevision(CompilationOwner.MaterialCompileStatus.AuthoredRevision);
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this);
		}
		InvalidateMaterialCompilation(false, true);
		MarkPackageDirty();
		MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState);
		return true;
	}

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector4
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0, 0)));
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector4
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0)));
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		auto Candidate = Definition->Value;
		Candidate.GetTexture().Texture = Value;
		return SetParameterValue(Definition->Id, Candidate);
	}

	auto DMaterial::SetParameterValue(const FGuid& Id, const FMaterialParameterValue& Value) -> bool
	{
		if (!Id.IsValid()) return false;
		auto Entry = std::ranges::find(ParameterSchema, Id, &FMaterialParameterDefinition::Id);
		if (Entry == ParameterSchema.end()) return false;
		const bool bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
		auto Definition = *Entry;
		Definition.Value = Value;
		if (!ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return false;
		if (*Entry == Definition) return true;
		if (!bCooked)
		{
			std::vector<DMaterialExpressionParameter*> Owners;
			for (const auto& Expression : ExpressionCollection.Expressions)
				if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == Id)
				{
					if (Parameter->GetParameterDefinition() != *Entry) return false;
					Owners.push_back(Parameter);
				}
			if (Owners.empty()) return false;
			for (auto* Parameter : Owners)
			{
				const bool bApplied = Parameter->SetParameterDefinition(Definition);
				require(bApplied);
			}
		}

		*Entry = std::move(Definition);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
		GraphChanges.Publish(*this);
		return true;
	}

	auto DMaterial::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Scalar, OutValue,
			[](const FMaterialParameterValue& Value) { return Value.GetScalar(); });
	}

	auto DMaterial::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Vector4, OutValue,
			[](const FMaterialParameterValue& Value) { return FVector2(Value.GetVector4()); });
	}

	auto DMaterial::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Vector4, OutValue,
			[](const FMaterialParameterValue& Value) { return FVector3(Value.GetVector4()); });
	}

	auto DMaterial::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Texture, OutValue,
			[](const FMaterialParameterValue& Value) { return Value.GetTexture().Texture.Get(); });
	}

	auto DMaterial::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialGraphVersion::Serialize(Ar) || !FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (!Ar.HasError() && !IsTemplateObject() && Ar.IsSaving() && Ar.GetPurpose() == EArchivePurpose::AuthoredPackage)
		{
			std::string Error;
			if (!ValidateLoadedObjectGraph({}, Error)) Ar.Fail(EArchiveFailureCode::InvalidData, Error);
		}
	}

	auto DMaterial::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		for (auto& Definition : ParameterSchema) Definition.Value.AddReferencedObjects(Collector);
	}

	auto DMaterial::SerializeCooked(FArchive& Ar) -> void
	{
		if (Ar.IsSaving() && !GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& !DeriveExpressionParameterSchema(ExpressionCollection, ParameterSchema))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Cannot Cook an invalid material parameter schema.");
			return;
		}
		Super::SerializeCooked(Ar);
		if (Ar.HasError()) return;
		std::vector<FMaterialParameterDefinition> Loaded;
		std::vector<uint32> LoadedOrder;
		for (const auto Type : {EMaterialParameterType::Scalar, EMaterialParameterType::Vector2,
			EMaterialParameterType::Vector, EMaterialParameterType::Vector4, EMaterialParameterType::Texture})
		{
			std::vector<FMaterialParameterDefinition> Values;
			if (Ar.IsSaving())
				for (const auto& Definition : ParameterSchema)
					if (Definition.Type == Type) Values.push_back(Definition);
			const FName RecordName(std::format("Durin::CookedMaterialParameter{}", static_cast<uint32>(Type)));
			auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterial"),
				FName(std::format("TypedParameters{}", static_cast<uint32>(Type))),
				FArchiveLogicalTypeDescriptor::Array(FArchiveLogicalTypeDescriptor::Struct(RecordName))});
			uint64 Index = 0;
			SerializeBoundedSequence(Ar, Values, MaterialProgramMaxNodeCount,
				[&](FArchive& Inner, FMaterialParameterDefinition& Definition) {
					auto Element = EnterArchiveArrayElement(Inner, Index++);
					Inner.GetStructBaseline();
					auto Member = [&](const char* Name, auto& Value, FArchiveLogicalTypeDescriptor LogicalType) {
						auto Scope = EnterArchiveField(Inner, {RecordName, FName(Name), std::move(LogicalType)});
						Inner << Value;
					};
					Member("Id", Definition.Id, FArchiveLogicalTypeDescriptor::Guid());
					uint32 Order = Ar.IsSaving() ? static_cast<uint32>(std::ranges::find(ParameterSchema,
						Definition.Id, &FMaterialParameterDefinition::Id) - ParameterSchema.begin()) : 0;
					Member("Order", Order, FArchiveLogicalTypeDescriptor::Scalar(false, 32));
					if (Inner.IsLoading()) LoadedOrder.push_back(Order);
					Member("Name", Definition.Name, FArchiveLogicalTypeDescriptor::Name());
					{
						auto Scope = EnterArchiveField(Inner, {RecordName, FName("DisplayName"), FArchiveLogicalTypeDescriptor::String()});
						SerializeBoundedString(Inner, Definition.DisplayName, MaterialMaxParameterTextBytes);
					}
					Member("GroupName", Definition.GroupName, FArchiveLogicalTypeDescriptor::Name());
					Member("SortOrder", Definition.SortOrder, FArchiveLogicalTypeDescriptor::Scalar(true, 32));
					Member("Presentation", Definition.Presentation, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialParameterPresentation"), false, 8));
					Definition.Type = Type;
					if (Inner.IsLoading())
					{
						switch (Type)
						{
						case EMaterialParameterType::Scalar: Definition.Value = FMaterialParameterValue::MakeScalar(0); break;
						case EMaterialParameterType::Vector2: Definition.Value = FMaterialParameterValue::MakeVector2(FVector2(0)); break;
						case EMaterialParameterType::Vector: Definition.Value = FMaterialParameterValue::MakeVector(FVector3(0)); break;
						case EMaterialParameterType::Vector4: Definition.Value = FMaterialParameterValue::MakeVector4(FVector4(0)); break;
						case EMaterialParameterType::Texture: Definition.Value = FMaterialParameterValue::MakeTexture(nullptr); break;
						}
					}
					switch (Type)
					{
					case EMaterialParameterType::Scalar:
						Member("Value", Definition.Value.GetScalar(), FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						Member("HasRange", Definition.bHasRange, FArchiveLogicalTypeDescriptor::Scalar(false, 8));
						Member("Minimum", Definition.MinimumValue, FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						Member("Maximum", Definition.MaximumValue, FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						break;
					case EMaterialParameterType::Vector2:
						Member("X", Definition.Value.GetVector2().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector2().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Vector:
						Member("X", Definition.Value.GetVector().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Z", Definition.Value.GetVector().z, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Vector4:
						Member("X", Definition.Value.GetVector4().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector4().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Z", Definition.Value.GetVector4().z, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("W", Definition.Value.GetVector4().w, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Texture:
					{
						auto& Texture = Definition.Value.GetTexture();
						{
							auto Scope = EnterArchiveField(Inner, {RecordName, FName("Texture"), FArchiveLogicalTypeDescriptor::Object(FName("Durin::DTexture2D"))});
							DObject* Object = Texture.Texture.Get();
							SerializeArchiveObjectReference(Inner, Object);
							if (Object && !Object->IsA(DTexture2D::StaticClass())) Inner.Fail(EArchiveFailureCode::InvalidData, "Cooked texture parameter has a non-texture reference.");
							Texture.Texture = Cast<DTexture2D>(Object);
						}
						Member("MinFilter", Texture.SamplerState.MinFilter, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerMinFilter"), false, 8));
						Member("MagFilter", Texture.SamplerState.MagFilter, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerMagFilter"), false, 8));
						Member("AddressU", Texture.SamplerState.AddressU, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerAddressMode"), false, 8));
						Member("AddressV", Texture.SamplerState.AddressV, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerAddressMode"), false, 8));
						Member("Fallback", Texture.TextureFallback, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialTextureFallback"), false, 8));
						Member("Usage", Definition.TextureUsage, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::ETextureUsage"), false, 8));
						break;
					}
					}
				});
			if (Ar.HasError()) return;
			if (Ar.IsLoading()) Loaded.insert(Loaded.end(), Values.begin(), Values.end());
		}
		if (Ar.IsLoading())
		{
			if (Loaded.size() > MaterialProgramMaxNodeCount || Loaded.size() != LoadedOrder.size())
			{
				Ar.Fail(EArchiveFailureCode::LimitExceeded, "Cooked parameter schema exceeds its bound.");
				return;
			}
			std::vector<FMaterialParameterDefinition> Ordered(Loaded.size());
			std::vector<bool> Seen(Loaded.size(), false);
			for (size_t Index = 0; Index < Loaded.size(); ++Index)
			{
				const auto Order = LoadedOrder[Index];
				if (Order >= Loaded.size() || Seen[Order])
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid cooked parameter declaration order.");
					return;
				}
				Seen[Order] = true;
				Ordered[Order] = std::move(Loaded[Index]);
			}
			ParameterSchema = std::move(Ordered);
		}
		if (!Ar.HasError() && !ValidateMaterialParameterDefinitions(ParameterSchema))
			Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid generated cooked material parameter schema.");
	}

	auto DMaterial::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
		if (!ValidateMaterialStaticProperties(StaticProperties, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				Error = std::format(
					"Cooked Material '{}': required ProgramData field is missing.",
					GetObjectPath());
				MaterialCookDiagnostic = Error;
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			CompilationOwner.RenderLayer.CompiledProgram.reset();
			CompilationOwner.MaterialCompileDiagnostics.clear();
			MaterialCookDiagnostic = std::format(
				"Loaded cooked Material metadata for '{}'.", GetObjectPath());
			return;
		}
		const auto SchemaValidation = DeriveExpressionParameterSchema(ExpressionCollection, ParameterSchema);
		if (!SchemaValidation)
		{
			DURIN_ERROR("PostLoad '{}': unsupported material graph; rebuild this material.", GetObjectPath());
			return;
		}
		const FMaterialProgramValidationResult ProgramValidation =
			ValidateExpressionGraph(ExpressionCollection, GetExpressionOutputs(), &ObservedExpressionCode);
		if (!ProgramValidation)
		{
			Error = ProgramValidation.Diagnostics.empty()
				? "Material program validation failed."
				: ProgramValidation.Diagnostics.front().Message;
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		std::vector<FGuid> Ids;
		for (const auto& Expression : ExpressionCollection.Expressions) if (Expression) Ids.push_back(Expression->Id);
		GraphPresentation = SanitizeMaterialGraphPresentation(GraphPresentation, Ids);
		AdvanceRevision(MaterialProgramRevision);
		RequestProgramCompile(StaticProperties);
		PublishMaterialRenderProxyState();
		NotifyParameterChanges();
		GraphChanges.Publish(*this);
	}

	auto DMaterial::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticProperties")) InvalidateMaterialCompilation(false, true);
		if (Name == FName("ExpressionCollection") || (Name == FName("StaticProperties")
			&& CanonicalizeMaterialShaderProperties(StaticProperties) != CompilationOwner.LastObservedShaderProperties))
		{
			if (Name == FName("ExpressionCollection"))
			{
				std::vector<FMaterialParameterDefinition> Schema;
				FXxHash128 Code;
				if (!DeriveExpressionParameterSchema(ExpressionCollection, Schema)
					|| !ValidateExpressionGraph(ExpressionCollection, GetExpressionOutputs(), &Code)) return;
				ParameterSchema = std::move(Schema);
				const bool bShaderChanged = Code != ObservedExpressionCode;
				ObservedExpressionCode = Code;
				if (!bShaderChanged)
				{
					MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
					GraphChanges.Publish(*this);
					return;
				}
				AdvanceRevision(MaterialProgramRevision);
			}
			AdvanceAuthoredRevision();
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this);
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap, Name == FName("ExpressionCollection"));
		}
		GraphChanges.Publish(*this);
	}

	auto DMaterial::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		Super::BeginDestroy();
	}
}
