#include "MaterialTranslationCodec.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::MaterialBuildPrivate
{
	namespace
	{
		// One bounded field walk defines both directions; no native struct bytes.
		template<bool Reading> struct FTransfer
		{
			FBinaryWriter* Writer = nullptr;
			FBinaryReader* Reader = nullptr;
			bool Good = true;
			template<class T> auto Value(T& V) -> void
			{
				using VType = std::remove_cv_t<T>;
				if constexpr (std::is_enum_v<VType>)
				{ uint32 Raw = uint32(V); Value(Raw); Good &= Raw <= std::numeric_limits<std::underlying_type_t<VType>>::max(); if constexpr (Reading) V = T(Raw); }
				else if constexpr (std::same_as<VType, bool>)
				{ uint32 Raw = V ? 1 : 0; Value(Raw); Good &= Raw <= 1; if constexpr (Reading) V = Raw != 0; }
				else if constexpr (std::same_as<VType, float>)
				{ if constexpr (!Reading) Writer->WriteFloat(V == 0.f ? 0.f : V); else Good &= Reader->ReadFloat(V); }
				else if constexpr (std::same_as<VType, uint64>)
				{ if constexpr (!Reading) Writer->WriteU64(V); else Good &= Reader->ReadU64(V); }
				else if constexpr (std::same_as<VType, uint32>)
				{ if constexpr (!Reading) Writer->WriteU32(V); else Good &= Reader->ReadU32(V); }
				else if constexpr (std::integral<VType>)
				{ uint32 Raw = V; Value(Raw); Good &= Raw <= std::numeric_limits<VType>::max(); if constexpr (Reading) V = T(Raw); }
				else if constexpr (std::same_as<VType, FGuid>)
				{ if constexpr (!Reading) Writer->WriteGuid(V); else Good &= Reader->ReadGuid(V); }
				else if constexpr (std::same_as<VType, FXxHash128>)
				{ if constexpr (!Reading) Writer->WriteHash128(V); else Good &= Reader->ReadHash128(V); }
			}
			template<class T> auto String(T& Text, uint64 Limit = MaterialProgramMaxStringBytes) -> void
			{ Good &= Text.size() <= Limit; if constexpr (!Reading) Writer->WriteString(Text); else Good &= Reader->ReadString(Text, Limit); }
			template<class T, class F> auto Sequence(T& Items, uint32 Limit, F Visit) -> void
			{
				uint32 Count = uint32(Items.size()); Value(Count);
				if (!Good || Count > Limit || (Reader && Count > Reader->GetRemainingBytes())) { Good = false; return; }
				if constexpr (Reading) Items.resize(Count);
				for (auto& Item : Items) Visit(Item);
			}
			template<class T> auto Optional(T& Item) -> void
			{ bool Present = Item.has_value(); Value(Present); if (Present) { if constexpr (Reading) Item.emplace(); Value(*Item); } else if constexpr (Reading) Item.reset(); }
			template<class T> auto Literal(T& Literal) -> void { Value(Literal.X); Value(Literal.Y); Value(Literal.Z); Value(Literal.W); }
			template<class T> auto Payload(T& V) -> void
			{
				using VType = std::remove_cv_t<T>;
				if constexpr (std::same_as<VType, FMaterialProgramLiteral>) Literal(V);
				else if constexpr (std::same_as<VType, FGuid>) Value(V);
				else if constexpr (std::same_as<VType, MIR::FCustomDataRead>) Value(V.Index);
				else if constexpr (std::same_as<VType, MIR::FSwizzle>) { Value(V.Length); for (auto& Part : V.Components) Value(Part); }
				else if constexpr (std::same_as<VType, MIR::FCollectionParameter>) { Value(V.CollectionId); Value(V.ParameterId); }
				else if constexpr (std::same_as<VType, FMaterialTransformPayload>) { Value(V.Source); Value(V.Destination); }
				else static_assert(std::same_as<VType, std::monostate>);
			}
			template<size_t Index = 0, class V, class F> auto Variant(V& Item, uint32 Kind, F Visit) -> void
			{
				if constexpr (Index < std::variant_size_v<std::remove_cv_t<V>>)
				{ if (Kind == Index) { if constexpr (Reading) Item.template emplace<Index>(); Visit(std::get<Index>(Item)); } else Variant<Index + 1>(Item, Kind, Visit); }
				else Good = false;
			}
			template<class T> auto Node(T& Node) -> void
			{
				Value(Node.Opcode); Value(Node.ResultType); Value(Node.LegalStages); Value(Node.SpatialKind); Value(Node.CoordinateSpace); Value(Node.ScalarBroadcastMask);
				Sequence(Node.Inputs, MaterialProgramMaxNodeInputCount, [&](auto& V) { Value(V); });
				uint32 Kind = uint32(Node.Payload.index()); Value(Kind); Variant(Node.Payload, Kind, [&](auto& V) { Payload(V); });
			}
			template<class T> auto Surface(T& Input) -> void
			{ Value(Input.bExpression); Value(Input.ExpressionIndex); Value(Input.Type); Value(Input.LegalStages); Value(Input.SpatialKind); Value(Input.CoordinateSpace); Literal(Input.Literal); }
			template<class T> auto IR(T& IR) -> void
			{
				Value(IR.Version); Sequence(IR.Nodes, MaterialFunctionMaxExpandedNodes, [&](auto& V) { Node(V); });
				Value(IR.SurfaceRoot.bAggregate); Value(IR.SurfaceRoot.AggregateExpressionIndex);
				for (auto& Input : IR.SurfaceRoot.Inputs) Surface(Input);
				Surface(IR.WorldPositionOffset);
			}
			template<class T> auto Parameters(T& Items) -> void
			{ Sequence(Items, MaterialRenderMaxFieldCount, [&](auto& V) { Value(V.Id); Value(V.Type); }); }
			template<class T> auto Layout(T& Layout) -> void
			{
				Value(Layout.Identity.Version); Value(Layout.Identity.Id); Value(Layout.UniformPayloadSize); Value(Layout.UniformFieldCount); Value(Layout.ResourceFieldCount);
				Sequence(Layout.Fields, MaterialRenderMaxFieldCount, [&](auto& Field) { Value(Field.ParameterId); Value(Field.Storage); Value(Field.Type); Value(Field.CompactIndex); Value(Field.Offset); Value(Field.Size); });
			}
			template<class T> auto Collections(T& Items) -> void
			{ Sequence(Items, MaterialParameterCollectionMaxPerMaterial, [&](auto& V) { Value(V.CollectionId); Value(V.SchemaVersion); Layout(V.UniformLayout); }); }
			template<class T> auto Properties(T& Properties) -> void
			{ Value(Properties.GeometryUsage); Value(Properties.PassUsage); Value(Properties.BlendMode); Value(Properties.ShadingModel); Value(Properties.OpacityMaskThreshold); }
			template<class T> auto Limits(T& Limits) -> void
			{ Value(Limits.SampledImages); Value(Limits.Samplers); Value(Limits.UniformBuffers); Value(Limits.StageResources); Value(Limits.UniformBufferBytes); }
			template<class T> auto Source(T& V) -> void { Value(V.ExpressionIndex); Value(V.NodeId); Optional(V.InputIndex); Optional(V.UVFieldIndex); }
			template<class T> auto Sources(T& Items) -> void
			{
				Sequence(Items, MaterialFunctionMaxExpandedNodes, [&](auto& V) { Source(V); });
			}
			template<class T> auto Input(T& Input) -> void
			{
				uint32 Version = TranslationSchema; Value(Version); Good &= Version == TranslationSchema;
				IR(Input.IR); Parameters(Input.Parameters); Collections(Input.Collections); Properties(Input.StaticProperties); Limits(Input.Environment.ResourceLimits);
				Value(Input.Environment.bIncludeEditorPasses); Value(Input.Environment.Quality); Value(Input.Environment.FeatureLevel);
				Sequence(Input.Environment.StaticBools, MaterialMaxStaticBoolDeclarations, [&](auto& V) { Value(V.DeclarationId); Value(V.Value); });
				Sources(Input.Sources);
			}
			template<class T> auto Diagnostic(T& D) -> void
			{
				Value(D.Category); Value(D.LocationKind); Value(D.NodeId); Value(D.LocationIndex); Optional(D.InputIndex); Optional(D.UVFieldIndex);
				uint32 Kind = uint32(D.Error.Code.index()); Value(Kind);
				Variant(D.Error.Code, Kind, [&](auto& Code) {
					using CodeType = std::decay_t<decltype(Code)>;
					if constexpr (!std::same_as<CodeType, std::monostate>) Value(Code);
					if constexpr (std::same_as<CodeType, EMaterialIRError>) Good &= uint32(Code) <= uint32(EMaterialIRError::UnsupportedGenerationNode);
					else if constexpr (std::same_as<CodeType, EMaterialPropertyError>) Good &= uint32(Code) <= uint32(EMaterialPropertyError::InvalidShaderUsage);
					else if constexpr (std::same_as<CodeType, EMaterialLayoutError>) Good &= uint32(Code) > uint32(EMaterialLayoutError::None) && uint32(Code) <= uint32(EMaterialLayoutError::InvalidReflection);
					else Good = false; // Only errors produced by detached translation may persist.
				});
				Optional(D.Error.Index); Value(D.Error.ParameterId); String(D.Error.ParameterName);
				Optional(D.Error.ExpectedParameterType); Optional(D.Error.ActualParameterType); Optional(D.Error.ExpectedType); Optional(D.Error.ActualType);
				Good &= D.Error.ExternalDiagnostic.empty() && !D.Error.ResourceCause && !D.Error.DuplicationCause && !D.Error.ArchiveCode && !D.Error.BulkStatus && !D.Error.ResourceStatus;
				Good &= uint32(D.Category) <= uint32(EMaterialProgramDiagnosticCategory::Binding) && uint32(D.LocationKind) <= uint32(EMaterialProgramDiagnosticLocationKind::SurfaceOutput);
			}
			template<class T> auto Product(T& Product, FXxHash128& InputIdentity, std::optional<FXxHash128> Semantic = {}) -> void
			{
				uint32 Version = TranslationSchema; Value(Version); Good &= Version == TranslationSchema;
				Value(InputIdentity); bool Succeeded = Product.Status == EMaterialProgramStatus::Ok; Value(Succeeded);
				if constexpr (Reading) Product.Status = Succeeded ? EMaterialProgramStatus::Ok : EMaterialProgramStatus::Error;
				auto& Result = Product;
				Value(Product.RequiredStages); IR(Result.IR); Parameters(Result.ActiveParameters); Collections(Result.ActiveCollections); Layout(Result.Layout); Properties(Product.StaticProperties); Limits(Product.ResourceLimits);
				String(Result.GeneratedSource, 1024 * 1024); Sequence(Product.Sources, MaterialFunctionMaxExpandedNodes, [&](auto& Location) { Source(Location.Source); });
				auto& Q = Result.Requirements;
				Value(Q.bMaterialView); Value(Q.bMaterialPrimitive); Value(Q.bCameraPosition); Value(Q.bViewport); Value(Q.bViewTransforms); Value(Q.bObjectTransforms); Value(Q.bBoundsCenter); Value(Q.bTangentFrame); Value(Q.bVertexNormal);
				Sequence(Result.Diagnostics, MaterialProgramMaxDiagnosticCount, [&](auto& V) { Diagnostic(V); });
				if constexpr (!Reading) { if (Semantic) Value(*Semantic); else Value(Result.Identity.Digest); } else Value(Result.Identity.Digest);
			}
			auto Valid() const -> bool { return Good && (Writer ? !Writer->HasError() : Reader->IsAtEnd()); }
		};
		auto ProducerEnvironment(MIR::FCompilerInput& Input) -> void
		{
			Input.Environment.CompilerIdentity = "Material.Translate";
			Input.Environment.Target = "vulkan-spirv-1.5";
			Input.Environment.BuilderVersion = GetMaterialBuildVersion();
			Input.Environment.AdapterVersion = GetMaterialCompileAdapterVersion();
			Input.Environment.Dependencies.clear();
		}
		auto SemanticIdentity(const FMaterialTranslatedProgram& Product) -> FMaterialProgramIdentity
		{
			FBinaryWriter Writer({.MaximumTotalBytes = TranslationOutputBytes}); FTransfer<false> Transfer{.Writer = &Writer};
			auto& Result = Product;
			Transfer.IR(Result.IR); Transfer.Parameters(Result.ActiveParameters); Transfer.Collections(Result.ActiveCollections); Transfer.Layout(Result.Layout);
			Transfer.Properties(Product.StaticProperties); Transfer.Limits(Product.ResourceLimits); Transfer.String(Result.GeneratedSource, 1024 * 1024);
			Writer.WriteU64(GetMaterialBuildVersion()); Writer.WriteU32(CurrentMaterialPassContractVersion);
			return {.Digest = FXxHash128::HashBuffer(Writer.GetBytes())};
		}
		auto Remap(FMaterialProgramDiagnostic& D, const MIR::FCompilerInput& Consumer) -> bool
		{
			if (!D.NodeId.IsValid()) return true;
			if (D.NodeId.A || D.NodeId.B || D.NodeId.C || !D.NodeId.D || D.NodeId.D > Consumer.Sources.size()) return false;
			const auto& Source = Consumer.Sources[D.NodeId.D - 1];
			D.NodeId = Source.NodeId; D.PortId = Source.PortId; D.FunctionAssetPath = Source.FunctionAssetPath; D.CallPath = Source.CallPath;
			return true;
		}
	}
	auto EstimateTranslationCaptureBytes(const MIR::FCompilerInput& Input) -> uint64
	{
		uint64 Bytes = sizeof(Input) + Input.IR.Nodes.capacity() * sizeof(MIR::FNode) + Input.Parameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Input.Sources.capacity() * sizeof(MIR::FSource) + Input.Environment.CompilerIdentity.capacity() + Input.Environment.Target.capacity()
			+ Input.Environment.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue);
		for (const auto& Node : Input.IR.Nodes) Bytes += Node.Inputs.capacity() * sizeof(uint32);
		for (const auto& Source : Input.Sources) Bytes += Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
		for (const auto& Collection : Input.Collections) Bytes += sizeof(Collection) + Collection.AssetPath.capacity() + Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
		for (const auto& Dependency : Input.Environment.Dependencies) Bytes += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		return Bytes;
	}
	auto EncodeTranslationInput(const MIR::FCompilerInput& Captured) -> std::optional<FSharedByteBuffer>
	{
		if (EstimateTranslationCaptureBytes(Captured) > TranslationInputBytes) return {};
		auto Parameters = Captured.Parameters;
		std::ranges::sort(Parameters, {}, &FMaterialCompilerParameterDeclaration::Id);
		std::vector<FMaterialParameterCollectionLayout> Collections;
		for (const auto& Item : Captured.Collections) Collections.push_back({.CollectionId = Item.CollectionId, .UniformLayout = Item.UniformLayout, .SchemaVersion = Item.SchemaVersion});
		std::ranges::sort(Collections, {}, &FMaterialParameterCollectionLayout::CollectionId);
		const auto Properties = CanonicalizeMaterialShaderProperties(Captured.StaticProperties);
		std::vector<MIR::FSource> Sources;
		for (uint32 Index = 0; Index < Captured.Sources.size(); ++Index)
		{
			const auto& Source = Captured.Sources[Index];
			Sources.push_back({.ExpressionIndex = Source.ExpressionIndex, .NodeId = FGuid(0, 0, 0, Index + 1), .InputIndex = Source.InputIndex, .UVFieldIndex = Source.UVFieldIndex});
		}
		struct FInputView
		{
			const MIR::FModule& IR;
			const std::vector<FMaterialCompilerParameterDeclaration>& Parameters;
			const std::vector<FMaterialParameterCollectionLayout>& Collections;
			const FMaterialStaticProperties& StaticProperties;
			const FMaterialCompilerEnvironment& Environment;
			const std::vector<MIR::FSource>& Sources;
		};
		const FInputView Input{Captured.IR, Parameters, Collections, Properties, Captured.Environment, Sources};
		FBinaryWriter Writer({.MaximumTotalBytes = TranslationInputBytes}); FTransfer<false> Transfer{.Writer = &Writer}; Transfer.Input(Input);
		if (!Transfer.Valid()) return {};
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	auto DecodeTranslationInput(FByteView Bytes) -> std::optional<MIR::FCompilerInput>
	{
		FBinaryReader Reader(Bytes, {.MaximumTotalBytes = TranslationInputBytes}); FTransfer<true> Transfer{.Reader = &Reader}; MIR::FCompilerInput Input;
		Transfer.Input(Input); if (!Transfer.Valid()) return {}; ProducerEnvironment(Input); return Input;
	}
	auto EncodeTranslationProduct(const FMaterialTranslatedProgram& Product, FXxHash128 Identity) -> std::optional<FSharedByteBuffer>
	{
		if (Product.Status == EMaterialProgramStatus::Canceled) return {};
		const auto Digest = Product ? SemanticIdentity(Product).Digest : Product.Identity.Digest;
		FBinaryWriter Writer({.MaximumTotalBytes = TranslationOutputBytes}); FTransfer<false> Transfer{.Writer = &Writer}; Transfer.Product(Product, Identity, Digest);
		if (!Transfer.Valid()) return {}; return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	auto DecodeTranslationProduct(FByteView Bytes, FXxHash128 InputIdentity, const MIR::FCompilerInput& Consumer) -> std::optional<FMaterialTranslatedProgram>
	{
		FBinaryReader Reader(Bytes, {.MaximumTotalBytes = TranslationOutputBytes}); FTransfer<true> Transfer{.Reader = &Reader}; FMaterialTranslatedProgram Product; FXxHash128 Identity;
		Transfer.Product(Product, Identity); if (!Transfer.Valid() || Identity != InputIdentity) return {};
		auto& Result = Product;
		if (Product.RequiredStages != SelectMaterialShaderStages(Consumer.StaticProperties, Consumer.Environment.bIncludeEditorPasses)) return {};
		if (Product.ResourceLimits != Consumer.Environment.ResourceLimits || Product.StaticProperties != CanonicalizeMaterialShaderProperties(Consumer.StaticProperties)) return {};
		if (Product)
		{
			if (!Result.Diagnostics.empty() || Result.GeneratedSource.empty() || !MIR::Validate(Result.IR, Result.ActiveParameters)) return {};
			if (Result.Requirements != BuildMaterialProgramRequirements(Result.IR)
				|| !std::ranges::is_sorted(Result.ActiveParameters, {}, &FMaterialCompilerParameterDeclaration::Id)
				|| !std::ranges::is_sorted(Result.ActiveCollections, {}, &FMaterialParameterCollectionLayout::CollectionId)) return {};
			for (const auto& Parameter : Result.ActiveParameters) if (!std::ranges::contains(Consumer.Parameters, Parameter)) return {};
			const auto Layout = CompileMaterialLayout(Result.ActiveParameters, Product.ResourceLimits);
			if (!Layout || Layout.Layout != Result.Layout) return {};
			FByteBuffer Canonical;
			if (!MIR::EncodeCanonical(Result.IR, Canonical)) return {};
			if (SemanticIdentity(Product) != Result.Identity) return {};
			Result.Identity = BuildMaterialProgramIdentity(Consumer.StaticProperties, Consumer.Environment, Result.ActiveCollections, Canonical, Result.Layout);
			for (auto& Collection : Result.ActiveCollections)
			{
				const auto Current = std::ranges::find(Consumer.Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
				if (Current == Consumer.Collections.end() || !Collection.HasCompatibleSchema(*Current)) return {};
				Collection.AssetPath = Current->AssetPath; Collection.DefaultPayload = Current->DefaultPayload;
			}
		}
		else if (Result.Diagnostics.empty()) return {};
		for (auto& Location : Product.Sources)
		{
			auto& Source = Location.Source;
			if (Source.NodeId.A || Source.NodeId.B || Source.NodeId.C || !Source.NodeId.D || Source.NodeId.D > Consumer.Sources.size() || Source.ExpressionIndex >= Result.IR.Nodes.size()) return {};
			Location.Ordinal = Source.NodeId.D - 1;
			const auto Index = Source.ExpressionIndex; Source = Consumer.Sources[Source.NodeId.D - 1]; Source.ExpressionIndex = Index;
		}
		for (auto& D : Result.Diagnostics) if (!Remap(D, Consumer)) return {};
		Result.CompilerIdentity = Consumer.Environment.CompilerIdentity; Result.Target = Consumer.Environment.Target; Result.PassContractVersion = Consumer.Environment.PassContractVersion;
		Result.Quality = Consumer.Environment.Quality; Result.FeatureLevel = Consumer.Environment.FeatureLevel; Result.StaticBools = Consumer.Environment.StaticBools; Result.Dependencies = Consumer.Environment.Dependencies;
		return Product;
	}
}
