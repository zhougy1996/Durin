#include "AssetSystem.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Engine/Engine.h"
#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/ReflectedPropertyView.h"
#include "DObject/Class.h"
#include "Workspace/LevelEditorContext.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Scene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialPreview.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint8 MaterialTexturePngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteMaterialTextureFixture(const std::filesystem::path& Path) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(MaterialTexturePngBytes), sizeof(MaterialTexturePngBytes));
	}

	auto RewriteSerializedFieldAsLegacyMap(
		std::vector<Durin::uint8>& Bytes,
		std::string_view CurrentName,
		std::string_view LegacyName
	) -> bool
	{
		const Durin::uint64 CurrentNameSize = CurrentName.size();
		size_t NameLengthOffset = std::string::npos;
		for (size_t Offset = 0; Offset + sizeof(CurrentNameSize) + CurrentName.size() <= Bytes.size(); ++Offset)
		{
			Durin::uint64 CandidateSize = 0;
			std::memcpy(&CandidateSize, Bytes.data() + Offset, sizeof(CandidateSize));
			if (CandidateSize == CurrentNameSize
				&& std::memcmp(Bytes.data() + Offset + sizeof(CandidateSize), CurrentName.data(), CurrentName.size()) == 0)
			{
				if (NameLengthOffset != std::string::npos) return false;
				NameLengthOffset = Offset;
			}
		}
		if (NameLengthOffset == std::string::npos) return false;

		const size_t NameOffset = NameLengthOffset + sizeof(Durin::uint64);
		Bytes.erase(Bytes.begin() + NameOffset, Bytes.begin() + NameOffset + CurrentName.size());
		Bytes.insert(Bytes.begin() + NameOffset, LegacyName.begin(), LegacyName.end());
		const Durin::uint64 LegacyNameSize = LegacyName.size();
		std::memcpy(Bytes.data() + NameLengthOffset, &LegacyNameSize, sizeof(LegacyNameSize));

		const size_t KindOffset = NameOffset + LegacyName.size();
		if (KindOffset + 1 + sizeof(Durin::uint64) > Bytes.size()) return false;
		Bytes[KindOffset] = static_cast<Durin::uint8>(Durin::DurinCodeGen::EPropertyGenFlags::Map);

		const size_t SignatureLengthOffset = KindOffset + 1;
		Durin::uint64 CurrentSignatureSize = 0;
		std::memcpy(&CurrentSignatureSize, Bytes.data() + SignatureLengthOffset, sizeof(CurrentSignatureSize));
		const size_t SignatureOffset = SignatureLengthOffset + sizeof(CurrentSignatureSize);
		if (CurrentSignatureSize > Bytes.size() - SignatureOffset) return false;

		constexpr std::string_view LegacySignature = "Map<legacy-string,legacy-value>";
		Bytes.erase(Bytes.begin() + SignatureOffset, Bytes.begin() + SignatureOffset + CurrentSignatureSize);
		Bytes.insert(Bytes.begin() + SignatureOffset, LegacySignature.begin(), LegacySignature.end());
		const Durin::uint64 LegacySignatureSize = LegacySignature.size();
		std::memcpy(Bytes.data() + SignatureLengthOffset, &LegacySignatureSize, sizeof(LegacySignatureSize));
		return true;
	}

	auto ContainsSerializedField(std::span<const Durin::uint8> Bytes, std::string_view Name) -> bool
	{
		const Durin::uint64 NameSize = Name.size();
		for (size_t Offset = 0; Offset + sizeof(NameSize) + Name.size() <= Bytes.size(); ++Offset)
		{
			Durin::uint64 CandidateSize = 0;
			std::memcpy(&CandidateSize, Bytes.data() + Offset, sizeof(CandidateSize));
			if (CandidateSize == NameSize
				&& std::memcmp(Bytes.data() + Offset + sizeof(CandidateSize), Name.data(), Name.size()) == 0)
			{
				return true;
			}
		}
		return false;
	}

	auto ReplaceAll(std::string& Text, std::string_view From, std::string_view To) -> void
	{
		size_t Offset = 0;
		while ((Offset = Text.find(From, Offset)) != std::string::npos)
		{
			Text.replace(Offset, From.size(), To);
			Offset += To.size();
		}
	}

	auto WriteStaticMeshSlotVariant(
		const std::filesystem::path& Path,
		std::string_view MaterialDeclarations,
		std::optional<std::pair<std::string_view, std::string_view>> PrimitiveReplacement = std::nullopt,
		bool bReplaceLastOnly = false,
		std::optional<Durin::uint32> AppendedMaterialIndex = std::nullopt) -> void
	{
		std::ifstream Input(std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf");
		ASSERT_TRUE(Input.is_open());
		std::string Text((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
		const size_t MaterialsBegin = Text.find("\"materials\": [");
		const size_t MaterialsEnd = Text.find("],", MaterialsBegin);
		ASSERT_NE(MaterialsBegin, std::string::npos);
		ASSERT_NE(MaterialsEnd, std::string::npos);
		const size_t ContentBegin = Text.find('[', MaterialsBegin) + 1;
		Text.replace(ContentBegin, MaterialsEnd - ContentBegin, MaterialDeclarations);
		if (PrimitiveReplacement)
		{
			if (bReplaceLastOnly)
			{
				const size_t Offset = Text.rfind(PrimitiveReplacement->first);
				ASSERT_NE(Offset, std::string::npos);
				Text.replace(Offset, PrimitiveReplacement->first.size(), PrimitiveReplacement->second);
			}
			else
			{
				ReplaceAll(Text, PrimitiveReplacement->first, PrimitiveReplacement->second);
			}
		}
		if (AppendedMaterialIndex)
		{
			const size_t PrimitiveBegin = Text.find("{ \"attributes\"");
			const size_t PrimitiveEnd = Text.find('\n', PrimitiveBegin);
			ASSERT_NE(PrimitiveBegin, std::string::npos);
			ASSERT_NE(PrimitiveEnd, std::string::npos);
			std::string Primitive = Text.substr(PrimitiveBegin, PrimitiveEnd - PrimitiveBegin);
			if (!Primitive.empty() && Primitive.back() == ',') Primitive.pop_back();
			const size_t MaterialOffset = Primitive.rfind("\"material\": ");
			ASSERT_NE(MaterialOffset, std::string::npos);
			const size_t ValueOffset = MaterialOffset + std::string_view("\"material\": ").size();
			Primitive.replace(ValueOffset, 1, std::to_string(*AppendedMaterialIndex));
			const size_t PrimitivesEnd = Text.find("\n      ]", PrimitiveEnd);
			ASSERT_NE(PrimitivesEnd, std::string::npos);
			Text.insert(PrimitivesEnd, ",\n        " + Primitive);
		}
		std::ofstream Output(Path, std::ios::trunc);
		ASSERT_TRUE(Output.is_open());
		Output << Text;
		ASSERT_TRUE(Output.good());
	}

	class FMaterialTestEngine final : public Durin::DEngine
	{
	public:
		FMaterialTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			auto Scene = std::make_unique<Durin::FScene>();
			Durin::FScene* Result = Scene.get();
			MainScene = std::move(Scene);
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
		auto SetTestRendererModule(Durin::IRendererModule* InRendererModule) -> void { RendererModule = InRendererModule; }
	};

	auto WaitForRenderingThread() -> void
	{
		Durin::FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	struct FSceneSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		Durin::FMaterialRenderData Material;
		Durin::FMatrix Transform{1.0};
		Durin::uint64 ComponentRevision = 0;
		Durin::uint64 MaterialVersion = 0;
		Durin::uint64 ProxyCount = 0;
	};

	auto CaptureScene(Durin::FScene* Scene) -> FSceneSnapshot
	{
		FSceneSnapshot Snapshot;
		struct FCaptureMaterialTestSceneCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialTestScene"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialTestSceneCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate& CommandList) {
			Snapshot.ProxyCount = Scene->GetPrimitiveSceneProxies().size();
			if (Scene->GetPrimitiveSceneProxies().empty()) return;
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Scene->GetPrimitiveSceneProxies().front());
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.Material = Snapshot.Proxy->GetMaterialRenderData();
			Snapshot.Transform = Snapshot.Proxy->GetLocalToWorld();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
			Snapshot.MaterialVersion = Snapshot.Proxy->GetMaterialVersion();
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	class FRenderSceneHarness
	{
	public:
		FRenderSceneHarness()
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
			Scene = Engine.CreateTestScene();
			Durin::GEngine = &Engine;
		}

		~FRenderSceneHarness() { Shutdown(); }

		auto Shutdown() -> void
		{
			if (!bActive) return;
			if (Scene != nullptr)
			{
				Scene->Release();
				WaitForRenderingThread();
				Engine.ResetTestScene();
				Scene = nullptr;
			}
			Durin::GEngine = nullptr;
			Durin::ShutdownRenderingThread();
			bActive = false;
		}

		FMaterialTestEngine Engine;
		Durin::FScene* Scene = nullptr;
		bool bActive = true;
	};

	class FMaterialPreviewHarness
	{
	public:
		FMaterialPreviewHarness()
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
			Engine.SetTestRendererModule(&RendererModule);
			Durin::GEngine = &Engine;
		}

		~FMaterialPreviewHarness()
		{
			Durin::GEngine = nullptr;
			WaitForRenderingThread();
			Durin::ShutdownRenderingThread();
		}

		FMaterialTestEngine Engine;
		Durin::FRendererModule RendererModule;
	};

	auto ExpectColorNear(const Durin::FVector4f& Actual, const Durin::FVector4f& Expected) -> void
	{
		EXPECT_NEAR(Actual.r, Expected.r, 1.e-6f);
		EXPECT_NEAR(Actual.g, Expected.g, 1.e-6f);
		EXPECT_NEAR(Actual.b, Expected.b, 1.e-6f);
		EXPECT_NEAR(Actual.a, Expected.a, 1.e-6f);
	}

	auto FindObjectByName(std::string_view Name) -> Durin::DObject*
	{
		const auto& Objects = Durin::GDObjectArray.GetAll();
		const auto It = std::ranges::find_if(Objects, [Name](const Durin::DObject* Object) {
			return Object && Object->GetName() == Name;
		});
		return It == Objects.end() ? nullptr : *It;
	}

	auto MakeMaterialValueTarget(Durin::DMaterial* Material, const Durin::FGuid& Id, Durin::FName FieldName)
		-> std::optional<Durin::FReflectedPropertyEditTarget>
	{
		Durin::FProperty* DefinitionsProperty = Material->GetClass()->FindPropertyByName("ParameterDefinitions");
		if (!DefinitionsProperty || DefinitionsProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Array) return std::nullopt;
		auto* Definitions = static_cast<Durin::FArrayProperty*>(DefinitionsProperty);
		if (!Definitions->GetInner()
			|| Definitions->GetInner()->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* DefinitionProperty = static_cast<Durin::FStructProperty*>(Definitions->GetInner());
		Durin::FProperty* ValueProperty = DefinitionProperty->GetStruct()->FindPropertyByName("Value");
		if (!ValueProperty || ValueProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* ValueStructProperty = static_cast<Durin::FStructProperty*>(ValueProperty);
		Durin::FProperty* Field = ValueStructProperty->GetStruct()->FindPropertyByName(FieldName);
		if (!Field) return std::nullopt;
		const std::span DefinitionsView = Material->GetParameterDefinitions();
		const auto It = std::ranges::find(DefinitionsView, Id, &Durin::FMaterialParameterDefinition::Id);
		if (It == DefinitionsView.end()) return std::nullopt;
		const Durin::uint64 Index = static_cast<Durin::uint64>(It - DefinitionsView.begin());
		void* Definition = Definitions->GetMutableElementPtr(Material, Index);
		void* Value = ValueProperty->GetValuePtr(Definition);
		return Durin::FReflectedPropertyEditTarget::ForMember(Material, Definitions)
			.ForArrayElement(Definitions->GetInner(), Index)
			.ForStructMember(ValueProperty)
			.ForStructMember(Field);
	}
}

TEST(FMaterialTests, RuntimeSchemaHasStableIdentityOrderAndMetadata)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "SchemaMaterial");
	const std::span Definitions = Material->GetParameterDefinitions();
	ASSERT_EQ(Definitions.size(), 5u);
	const std::array ExpectedIds{
		Durin::MaterialParameters::BaseColorId,
		Durin::MaterialParameters::BaseColorTextureId,
		Durin::MaterialParameters::OpacityId,
		Durin::MaterialParameters::SpecularStrengthId,
		Durin::MaterialParameters::ShininessId,
	};
	std::unordered_set<Durin::FGuid> Ids;
	std::unordered_set<Durin::FName> Names;
	for (size_t Index = 0; Index < Definitions.size(); ++Index)
	{
		const Durin::FMaterialParameterDefinition& Definition = Definitions[Index];
		EXPECT_EQ(Definition.Id, ExpectedIds[Index]);
		EXPECT_TRUE(Ids.insert(Definition.Id).second);
		EXPECT_TRUE(Names.insert(Definition.Name).second);
		EXPECT_FALSE(Definition.Name.IsNone());
		EXPECT_FALSE(Definition.DisplayName.empty());
		EXPECT_EQ(Definition.SortOrder, static_cast<Durin::int32>(Index));
		switch (Definition.Presentation)
		{
		case Durin::EMaterialParameterPresentation::Drag:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Scalar);
			EXPECT_TRUE(Definition.bHasRange);
			EXPECT_LT(Definition.MinimumValue, Definition.MaximumValue);
			break;
		case Durin::EMaterialParameterPresentation::Color:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Vector);
			break;
		case Durin::EMaterialParameterPresentation::AssetPicker:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Texture);
			break;
		case Durin::EMaterialParameterPresentation::Default: FAIL() << "Built-in parameters require an explicit presentation."; break;
		}
	}
	EXPECT_EQ(Material->FindParameterDefinition(Durin::MaterialParameters::OpacityId), &Definitions[2]);
	EXPECT_EQ(Material->FindParameterDefinition(Durin::FName("oPaCiTy")), &Definitions[2]);
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::BaseColorName(), 0.5f));
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.5f));
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, RuntimeSchemaValidationReportsSpecificCorruption)
{
	InitializeDObjectSystem();
	std::vector Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	std::string Error;
	EXPECT_TRUE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_TRUE(Error.empty());

	Definitions[1].Id = Definitions[0].Id;
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("duplicate GUID"), std::string::npos);

	Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	Definitions[2].Name = Durin::FName("RenamedOpacity");
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);

	Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	std::swap(Definitions[0], Definitions[1]);
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);

	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "CorruptedSchemaMaterial");
	auto* Property = static_cast<Durin::FArrayProperty*>(Material->GetClass()->FindPropertyByName("ParameterDefinitions"));
	ASSERT_NE(Property, nullptr);
	auto* Opacity = static_cast<Durin::FMaterialParameterDefinition*>(Property->GetMutableElementPtr(Material, 2));
	Opacity->Type = Durin::EMaterialParameterType::Vector;
	EXPECT_FALSE(Material->PostLoad(Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, DetailsMaterialAssignmentReplacesRegisteredProxyOnRenderThread)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstDetailsMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondDetailsMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "DetailsMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(First);
	Component->RegisterComponent();
	const FSceneSnapshot Before = CaptureScene(Harness.Scene);

	Durin::FProperty* MaterialProperty = Component->GetClass()->FindPropertyByName("Material");
	ASSERT_NE(MaterialProperty, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(MaterialProperty, Component, 0, Original));
	auto* ObjectProperty = static_cast<Durin::FObjectProperty*>(MaterialProperty);
	ObjectProperty->SetObjectPropertyValue(Component, Second);
	ASSERT_TRUE(Durin::CapturePropertyValue(MaterialProperty, Component, 0, Proposed));
	ASSERT_TRUE(Durin::RestorePropertyValue(MaterialProperty, Component, 0, Original));
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession EditSession;
	ASSERT_TRUE(EditSession.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Component, MaterialProperty),
		"Edit Material",
		nullptr,
		&Transactions
	));
	EXPECT_EQ(EditSession.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(EditSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	const FSceneSnapshot After = CaptureScene(Harness.Scene);

	EXPECT_NE(Before.Proxy, After.Proxy);
	ExpectColorNear(After.Material.BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	ASSERT_TRUE(Transactions.Undo());
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	EXPECT_NE(After.Proxy, Undone.Proxy);
	ExpectColorNear(Undone.Material.BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Transactions.Redo());
	const FSceneSnapshot Redone = CaptureScene(Harness.Scene);
	EXPECT_NE(Undone.Proxy, Redone.Proxy);
	ExpectColorNear(Redone.Material.BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	Transactions.Clear();

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedMaterialSlotEditUsesSharedTransactionsAndSetterSemantics)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstReflectedSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondReflectedSlotMaterial");
	Durin::DMaterial* Replacement = Durin::NewObject<Durin::DMaterial>(nullptr, "ReplacementReflectedSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.4, 0.5, 0.6));
	Replacement->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.8, 0.9));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "ReflectedSlotMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();

	Durin::FProperty* ReflectedMaterials = Component->GetClass()->FindPropertyByName("Materials");
	ASSERT_NE(ReflectedMaterials, nullptr);
	ASSERT_EQ(ReflectedMaterials->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Array);
	auto* MaterialsProperty = static_cast<Durin::FArrayProperty*>(ReflectedMaterials);
	ASSERT_NE(MaterialsProperty->GetInner(), nullptr);
	ASSERT_EQ(MaterialsProperty->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
	auto* MaterialProperty = static_cast<Durin::FObjectProperty*>(MaterialsProperty->GetInner());

	auto MakeTarget = [&](Durin::uint32 SlotIndex) {
		void* Element = SlotIndex < MaterialsProperty->Num(Component)
			? MaterialsProperty->GetMutableElementPtr(Component, SlotIndex) : Component;
		return Durin::FReflectedPropertyEditTarget::ForMember(Component, MaterialsProperty)
			.ForArrayElement(MaterialProperty, SlotIndex);
	};
	auto SubmitSlot = [&](Durin::FReflectedPropertyView& View, const Durin::FReflectedPropertyViewContext& Context,
		Durin::uint32 SlotIndex, Durin::DMaterialInterface* Material) {
		const Durin::FReflectedPropertyEditTarget MaterialsTarget =
			Durin::FReflectedPropertyEditTarget::ForMember(Component, MaterialsProperty);
		if (SlotIndex < MaterialsProperty->Num(Component))
		{
			const Durin::FReflectedPropertyEditTarget SlotTarget = MakeTarget(SlotIndex);
			return View.SubmitPropertyValueEdit(Context, SlotTarget,
				[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
					static_cast<Durin::FObjectProperty*>(ScratchProperty)->SetObjectPropertyValue(
						ScratchContainer, Material, ScratchArrayIndex);
			}, false);
		}
		return View.SubmitPropertyValueEdit(Context, MaterialsTarget,
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
				auto* ScratchMaterials = static_cast<Durin::FArrayProperty*>(ScratchProperty);
				ScratchMaterials->Resize(ScratchContainer, static_cast<Durin::uint64>(SlotIndex) + 1, ScratchArrayIndex);
				MaterialProperty->SetObjectPropertyValue(
					ScratchMaterials->GetMutableElementPtr(ScratchContainer, SlotIndex, ScratchArrayIndex), Material);
		}, false);
	};

	const Durin::FReflectedPropertyEditTarget SlotTarget = MakeTarget(1);
	ASSERT_EQ(SlotTarget.Path.size(), 2u);
	EXPECT_EQ(SlotTarget.MemberProperty, MaterialsProperty);
	EXPECT_EQ(SlotTarget.LeafProperty, MaterialProperty);
	EXPECT_EQ(SlotTarget.Path[0].Selector, Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(SlotTarget.Path[0].Index, 1u);

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView View;
	const Durin::FReflectedPropertyViewContext Context{.Transactions = &Transactions};
	const FSceneSnapshot Before = CaptureScene(Harness.Scene);
	ASSERT_TRUE(SubmitSlot(View, Context, 1, Replacement));
	EXPECT_EQ(Component->GetMaterial(0), First);
	EXPECT_EQ(Component->GetMaterial(1), Replacement);
	EXPECT_TRUE(Transactions.CanUndo());
	const FSceneSnapshot After = CaptureScene(Harness.Scene);
	EXPECT_NE(Before.Proxy, After.Proxy);
	ASSERT_NE(After.Proxy, nullptr);
	ExpectColorNear(After.Proxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(After.Proxy->GetMaterialRenderData(1).BaseColor, Durin::FVector4f(0.7f, 0.8f, 0.9f, 1.0f));

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterial(0), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	EXPECT_NE(After.Proxy, Undone.Proxy);
	ASSERT_NE(Undone.Proxy, nullptr);
	ExpectColorNear(Undone.Proxy->GetMaterialRenderData(1).BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Component->GetMaterial(1), Replacement);

	Transactions.Clear();
	ASSERT_TRUE(SubmitSlot(View, Context, 1, nullptr));
	EXPECT_EQ(Component->GetMaterial(1), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterial(1), Replacement);
	Transactions.Clear();
	EXPECT_FALSE(SubmitSlot(View, Context, 1, Replacement));
	EXPECT_FALSE(Transactions.CanUndo());
	ASSERT_TRUE(SubmitSlot(View, Context, 2, Replacement));
	EXPECT_EQ(Component->GetMaterial(2), Replacement);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetNumMaterials(), 2u);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedParameterEditCoalescesAndInvalidatesRenderDataAcrossUndoRedo)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "TransactionalMaterial");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::OpacityId, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	const Durin::uint64 BeforeVersion = Material->GetRenderStateVersion();
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.6f;
		}, true));
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.4f;
		}, true));
	EXPECT_GT(Material->GetRenderStateVersion(), BeforeVersion);
	PropertyView.FinishActiveEdit(&Context, false);
	EXPECT_TRUE(Error.empty());
	float Opacity = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.4f);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.4f);
	Transactions.Clear();
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPropertyViewTracksPresentedOwnerSeparatelyFromEditTarget)
{
	InitializeDObjectSystem();
	Durin::DMaterialInstance* Owner = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PropertyViewOwner");
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "PropertyViewTarget");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::OpacityId, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.5f;
		}, true));
	EXPECT_TRUE(PropertyView.IsEditingObject(Material));
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.IsEditing());

	PropertyView.HandleOwnerContext(Context, Material);
	EXPECT_FALSE(PropertyView.IsEditing());
	EXPECT_TRUE(Error.empty());
	float Opacity = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);

	Transactions.Clear();
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Owner);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPropertyViewTracksMaterialOverrideStructureInSharedHistory)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "TransactionalOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TransactionalOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	auto* Property = static_cast<Durin::FArrayProperty*>(Instance->GetClass()->FindPropertyByName("ParameterOverrides"));
	ASSERT_NE(Property, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Original));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f));
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Proposed));
	ASSERT_TRUE(Instance->ClearScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	Durin::FEditorTransactionManager Transactions;
	std::string Error;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Instance, Property),
		"Edit Parameter Override", nullptr, &Transactions));
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	Transactions.Clear();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, UnknownAndMismatchedSettersDoNotInvalidateRenderState)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "RejectedSetterMaterial");
	const Durin::uint64 Version = Material->GetRenderStateVersion();
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.25f));
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::BaseColorName(), 0.25f));
	EXPECT_EQ(Material->GetRenderStateVersion(), Version);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ParentHookRejectsCyclesWithoutCreatingHistory)
{
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleFirst");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleSecond");
	ASSERT_TRUE(First->SetParent(Second));
	Durin::FProperty* ParentProperty = Second->GetClass()->FindPropertyByName("Parent");
	ASSERT_NE(ParentProperty, nullptr);

	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Second, 0, Original));
	static_cast<Durin::FObjectProperty*>(ParentProperty)->SetObjectPropertyValue(Second, First);
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Second, 0, Proposed));
	ASSERT_TRUE(Durin::RestorePropertyValue(ParentProperty, Second, 0, Original));

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::FReflectedPropertyEditTarget::ForMember(Second, ParentProperty), "Edit Parent", nullptr, &Transactions));
	std::string Error;
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Error, "A material instance cannot create a parent cycle.");
	EXPECT_EQ(Second->GetParent(), nullptr);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
	EXPECT_FALSE(Transactions.CanUndo());

	First->SetParent(nullptr);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, BoundMaterialAndParentChangesUpdateProxyInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "LiveBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveMaterialInstance");
	EXPECT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "LiveMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);

	const Durin::uint64 VersionBefore = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	const FSceneSnapshot ParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(ParentChanged.Proxy, Initial.Proxy);
	EXPECT_GT(Base->GetRenderStateVersion(), VersionBefore);
	EXPECT_GT(ParentChanged.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(ParentChanged.MaterialVersion, Instance->GetRenderStateVersion());
	ExpectColorNear(ParentChanged.Material.BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 1.0f));

	const Durin::uint64 NoOpVersion = Base->GetRenderStateVersion();
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	EXPECT_EQ(Base->GetRenderStateVersion(), NoOpVersion);
	Instance->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	Instance->ClearVectorParameterValue(Durin::MaterialParameters::BaseColorName());
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.9, 0.1, 0.3));
	const FSceneSnapshot Final = CaptureScene(Harness.Scene);
	EXPECT_EQ(Final.Proxy, Initial.Proxy);
	ExpectColorNear(Final.Material.BaseColor, Durin::FVector4f(0.9f, 0.1f, 0.3f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Base->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, BoundTextureChangesUpdateProxyResourceSnapshotInPlace)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "LiveTextureBaseMaterial");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LiveTextureMaterialInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "LiveOverrideColorTexture");
	Base->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(Instance->SetParent(Base));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "LiveTextureMaterialComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	EXPECT_EQ(Initial.Material.BaseColorTexture, BaseTexture->GetRenderResource());

	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	FSceneSnapshot Overridden = CaptureScene(Harness.Scene);
	EXPECT_EQ(Overridden.Proxy, Initial.Proxy);
	EXPECT_GT(Overridden.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(Overridden.Material.BaseColorTexture, OverrideTexture->GetRenderResource());

	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	FSceneSnapshot Inherited = CaptureScene(Harness.Scene);
	EXPECT_EQ(Inherited.Proxy, Initial.Proxy);
	EXPECT_EQ(Inherited.Material.BaseColorTexture, BaseTexture->GetRenderResource());
	// Test snapshots cross back to the game thread, so release their proxy owners while each asset still owns its resource.
	Initial.Material.BaseColorTexture.reset();
	Overridden.Material.BaseColorTexture.reset();
	Inherited.Material.BaseColorTexture.reset();

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Harness.Shutdown();
	Durin::CollectGarbage();
	// DTexture2D destruction enqueues the final release; briefly restart the worker to drain it in render-thread context.
	Durin::InitRenderingThread();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, SceneCommandsPreserveLatestTransformAndReleaseAllProxies)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SceneCommandComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	Component->SetWorldLocation(Durin::FVector3(4.0, 5.0, 6.0));
	const FSceneSnapshot Updated = CaptureScene(Harness.Scene);
	EXPECT_EQ(Updated.ProxyCount, 1);
	EXPECT_NEAR(Updated.Transform[3][0], 4.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][1], 5.0, 1.e-6);
	EXPECT_NEAR(Updated.Transform[3][2], 6.0, 1.e-6);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);

	Harness.Scene->Release();
	WaitForRenderingThread();
	EXPECT_EQ(CaptureScene(Harness.Scene).ProxyCount, 0);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, InstancesInheritOverrideAndRejectParentCycles)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "BaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondInstance");

	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	ExpectColorNear(Second->GetRenderData().BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	First->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.4f);
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	ExpectColorNear(Second->GetRenderData().BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 0.4f));
	EXPECT_FALSE(First->SetParent(Second));
	EXPECT_EQ(First->GetParent(), Base);

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, MultiLevelResolutionReportsSupplyingSourceAndCurrentOverrideState)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "ResolutionBase");
	Durin::DMaterialInstance* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionParent");
	Durin::DMaterialInstance* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ResolutionChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));

	Durin::FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Base);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 1.0f);

	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.6f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Parent);
	EXPECT_FALSE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 0.6f);

	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Child->ResolveParameterValue(Durin::MaterialParameters::OpacityId, Resolved));
	EXPECT_EQ(Resolved.Source, Child);
	EXPECT_TRUE(Resolved.bHasLocalOverride);
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, 0.25f);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ParentRemovalPreservesOrphansAndExcludesThemFromRendering)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "OrphanBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_FALSE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));

	ASSERT_TRUE(Instance->SetParent(nullptr));
	ASSERT_EQ(Instance->GetParameterOverrides().size(), 1u);
	EXPECT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_TRUE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));
	Durin::FResolvedMaterialParameter Resolved;
	EXPECT_FALSE(Instance->ResolveParameterValue(Durin::MaterialParameters::BaseColorId, Resolved));
	ExpectColorNear(Instance->GetRenderData().BaseColor, Durin::FVector4f(0.95f, 0.62f, 0.22f, 1.0f));

	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::BaseColorId));
	ExpectColorNear(Instance->GetRenderData().BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Instance->ClearParameterOverride(Durin::MaterialParameters::BaseColorId));
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, GuidOverrideRejectsUnknownAndPreservesVersionOnNoOp)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "GuidOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "GuidOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const Durin::FGuid Unknown{1, 2, 3, 4};
	const Durin::uint64 InitialVersion = Instance->GetRenderStateVersion();
	EXPECT_FALSE(Instance->SetParameterOverride(
		Unknown, Durin::EMaterialParameterType::Scalar, Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	EXPECT_FALSE(Instance->SetParameterOverride(
		Durin::MaterialParameters::BaseColorId,
		Durin::EMaterialParameterType::Scalar,
		Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	EXPECT_EQ(Instance->GetRenderStateVersion(), InitialVersion);
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());

	ASSERT_TRUE(Instance->SetParameterOverride(
		Durin::MaterialParameters::OpacityId,
		Durin::EMaterialParameterType::Scalar,
		Durin::FMaterialParameterValue::MakeScalar(0.5f)));
	const Durin::uint64 OverriddenVersion = Instance->GetRenderStateVersion();
	Durin::FMaterialParameterValue SameActiveValue = Durin::FMaterialParameterValue::MakeScalar(0.5f);
	SameActiveValue.VectorValue = Durin::FVector3(9.0);
	ASSERT_TRUE(Instance->SetParameterOverride(
		Durin::MaterialParameters::OpacityId,
		Durin::EMaterialParameterType::Scalar,
		SameActiveValue));
	EXPECT_EQ(Instance->GetRenderStateVersion(), OverriddenVersion);
	EXPECT_EQ(Instance->GetParameterOverrides().size(), 1u);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, InstanceOverrideStateTracksSetAndClear)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "OverrideStateBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "OverrideStateInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f);
	Instance->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	EXPECT_TRUE(Instance->ClearScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Instance->ClearVectorParameterValue(Durin::MaterialParameters::BaseColorName()));
	EXPECT_TRUE(Instance->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_FALSE(Instance->HasVectorParameterOverride(Durin::MaterialParameters::BaseColorName()));
	EXPECT_FALSE(Instance->HasTextureParameterOverride(Durin::MaterialParameters::BaseColorTextureName()));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, TextureParametersInheritOverrideAndPreserveExplicitNull)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "TextureBaseMaterial");
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FirstTextureInstance");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SecondTextureInstance");
	Durin::DTexture2D* BaseTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "InheritedBaseColorTexture");
	Durin::DTexture2D* OverrideTexture = Durin::NewObject<Durin::DTexture2D>(nullptr, "OverriddenBaseColorTexture");

	Base->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), BaseTexture);
	ASSERT_TRUE(First->SetParent(Base));
	ASSERT_TRUE(Second->SetParent(First));
	EXPECT_EQ(Second->GetRenderData().BaseColorTexture, BaseTexture->GetRenderResource());

	First->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), OverrideTexture);
	EXPECT_EQ(Second->GetRenderData().BaseColorTexture, OverrideTexture->GetRenderResource());
	Second->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), nullptr);
	EXPECT_EQ(Second->GetRenderData().BaseColorTexture, nullptr);
	EXPECT_TRUE(Second->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(Second->GetRenderData().BaseColorTexture, OverrideTexture->GetRenderResource());
	EXPECT_TRUE(First->ClearTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName()));
	EXPECT_EQ(Second->GetRenderData().BaseColorTexture, BaseTexture->GetRenderResource());

	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OverrideTexture);
	Durin::MarkAsGarbage(BaseTexture);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, ReflectedTextureParameterKeepsTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "RootedTextureMaterial");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedMaterialTexture");
	Material->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture);
	Durin::AddToRoot(Material);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Material));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Material);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Material));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, ReflectedInstanceOverrideKeepsNestedTextureReachable)
{
	InitializeDObjectSystem();
	Durin::InitRenderingThread();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "RootedOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RootedOverrideInstance");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "ReferencedOverrideTexture");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture));
	Durin::AddToRoot(Instance);

	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Base));
	EXPECT_TRUE(Durin::GDObjectArray.Contains(Texture));

	Durin::RemoveFromRoot(Instance);
	Durin::CollectGarbage();
	WaitForRenderingThread();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Instance));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Base));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Texture));
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialTests, DuplicateInstancePreservesParentAndNestedTextureOverride)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "DuplicateOverrideBase");
	Durin::DMaterialInstance* Source = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "DuplicateOverrideSource");
	Durin::DTexture2D* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "DuplicateOverrideTexture");
	ASSERT_TRUE(Source->SetParent(Base));
	ASSERT_TRUE(Source->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), Texture));

	std::string Error;
	auto* Duplicate = Durin::Cast<Durin::DMaterialInstance>(
		Durin::DuplicateObjectGraph(Source, nullptr, "DuplicateOverrideResult", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	EXPECT_EQ(Duplicate->GetParent(), Base);
	EXPECT_TRUE(Duplicate->HasLocalParameterOverride(Durin::MaterialParameters::BaseColorTextureId));
	Durin::DTexture2D* DuplicateTexture = nullptr;
	ASSERT_TRUE(Duplicate->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), DuplicateTexture));
	EXPECT_EQ(DuplicateTexture, Texture);

	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MultiMaterialMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(1).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyUsesEmptyFallbackForUnassignedSlots)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "FallbackMeshComponent");
	Component->SetStaticMesh(Mesh);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const Durin::FMaterialRenderData& Fallback = StaticMeshProxy->GetMaterialRenderData(SlotIndex);
		ExpectColorNear(Fallback.BaseColor, Durin::FMaterialRenderData{}.BaseColor);
		EXPECT_EQ(Fallback.BaseColorTexture, nullptr);
		EXPECT_FLOAT_EQ(Fallback.SpecularStrength, Durin::FMaterialRenderData{}.SpecularStrength);
		EXPECT_FLOAT_EQ(Fallback.Shininess, Durin::FMaterialRenderData{}.Shininess);
	}

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, DebugStaticMeshProvidesCompleteLODAndPackedAttributes)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.Positions.size(), 3u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	for (const auto& Channel : LOD.TexCoords) EXPECT_EQ(Channel.size(), LOD.Positions.size());
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].IndexCount, 3u);

	std::array<Durin::FVector2f, Durin::MaxStaticMeshUVChannels> TexCoords{};
	const Durin::FStaticMeshPackedVertex Packed = Durin::PackStaticMeshVertex(
		Durin::FVector3f(0.0f, 0.0f, 1.0f), Durin::FVector4f(1.0f, 0.0f, 0.0f, -1.0f), TexCoords, Durin::FVector4f(1.0f, 0.5f, 0.0f, 0.25f));
	EXPECT_EQ(Packed.Normal[2], 32767);
	EXPECT_EQ(Packed.Tangent[0], 32767);
	EXPECT_EQ(Packed.Tangent[3], -32767);
	EXPECT_EQ(Packed.Color[0], 255);
	EXPECT_EQ(Packed.Color[1], 128);
	EXPECT_EQ(Packed.Color[2], 0);
	EXPECT_EQ(Packed.Color[3], 64);

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, EngineMaterialPreviewMeshesLoadAsTransientGeometry)
{
	InitializeDObjectSystem();
	const std::string PreviewContent = Durin::FPaths::EngineContentDir() + "Editor/MaterialPreview/";
	for (const std::string_view Name : {"Sphere", "Box"})
	{
		std::string Error;
		Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateTransientFromFile(
			PreviewContent + std::string(Name) + ".obj", nullptr, std::format("TestMaterialPreview{}", Name), Error);
		ASSERT_NE(Mesh, nullptr) << Error;
		const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		ASSERT_NE(RenderData, nullptr);
		ASSERT_EQ(RenderData->LODResources.size(), 1u);
		const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		EXPECT_GT(LOD.Positions.size(), 8u);
		EXPECT_GT(LOD.Indices.size(), 12u);
		EXPECT_EQ(LOD.NumTexCoords, 1u);
		EXPECT_EQ(LOD.TexCoords[0].size(), LOD.Positions.size());
		Durin::MarkAsGarbage(Mesh);
	}
	Durin::CollectGarbage();
}

TEST(FMaterialTests, MaterialPreviewResourcesRemainAliveAcrossGarbageCollection)
{
	FMaterialPreviewHarness Harness;

	constexpr Durin::uint64 PreviewId = 987654321;
	const std::string SphereName = std::format("MaterialPreviewSphere_{}", PreviewId);
	const std::string BoxName = std::format("MaterialPreviewBox_{}", PreviewId);
	const std::string LightName = std::format("MaterialPreviewLight_{}", PreviewId);
	{
		Durin::FMaterialPreview Preview(PreviewId);
		ASSERT_NE(FindObjectByName(SphereName), nullptr);
		ASSERT_NE(FindObjectByName(BoxName), nullptr);
		ASSERT_NE(FindObjectByName(LightName), nullptr);

		Durin::CollectGarbage();
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(FindObjectByName(SphereName));
		auto* Box = Durin::Cast<Durin::DStaticMesh>(FindObjectByName(BoxName));
		auto* Light = Durin::Cast<Durin::DDirectionalLightComponent>(FindObjectByName(LightName));
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		ASSERT_NE(Light, nullptr);
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage();
	EXPECT_EQ(FindObjectByName(SphereName), nullptr);
	EXPECT_EQ(FindObjectByName(BoxName), nullptr);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
}

TEST(FMaterialTests, ImportedStaticMeshBuildsLODSectionsAndMaterialSlots)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshImports";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/MeshImportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult ImportResult = Durin::DStaticMesh::ImportAsset(Source.generic_string(), "/MeshImportTests/MultiSection");
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	const Durin::FStaticMeshRenderData* RenderData = ImportResult.Asset->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 2u);
	EXPECT_EQ(RenderData->MaterialSlots[0].Name, "Red");
	EXPECT_EQ(RenderData->MaterialSlots[1].Name, "Blue");
	EXPECT_EQ(RenderData->MaterialSlots[0].SlotId, ImportResult.Asset->GetMaterialSlot(0)->SlotId);
	EXPECT_EQ(RenderData->MaterialSlots[1].SlotId, ImportResult.Asset->GetMaterialSlot(1)->SlotId);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.NumTexCoords, 2u);
	EXPECT_TRUE(LOD.bHasVertexColors);
	EXPECT_EQ(LOD.Positions.size(), 12u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Indices.size(), 12u);
	ASSERT_EQ(LOD.Sections.size(), 4u);
	for (size_t SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
	{
		const Durin::FStaticMeshSection& Section = LOD.Sections[SectionIndex];
		EXPECT_EQ(Section.FirstIndex, static_cast<Durin::uint32>(SectionIndex) * 3u);
		EXPECT_EQ(Section.IndexCount, 3u);
		EXPECT_EQ(Section.MaterialSlotIndex, static_cast<Durin::uint32>(SectionIndex % 2u));
		EXPECT_TRUE(Section.LocalBounds.bIsValid);
	}
	EXPECT_TRUE(LOD.LocalBounds.bIsValid);
	EXPECT_TRUE(RenderData->LocalBounds.bIsValid);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshImportTests/MultiSection", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FMaterialTests, StaticMeshMaterialSlotDefinitionsRoundTripWithDefaults)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshSlotRoundTrip";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotRoundTrip/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath MaterialPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotRoundTrip/Default", MaterialPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_EQ(Import.Asset->GetNumMaterialSlots(), 2u);
	const std::vector<Durin::FGuid> OriginalIds{
		Import.Asset->GetMaterialSlot(0)->SlotId,
		Import.Asset->GetMaterialSlot(1)->SlotId};
	EXPECT_TRUE(OriginalIds[0].IsValid());
	EXPECT_TRUE(OriginalIds[1].IsValid());
	EXPECT_NE(OriginalIds[0], OriginalIds[1]);
	EXPECT_EQ(Import.Asset->FindMaterialSlot(OriginalIds[1]), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->FindMaterialSlot(Durin::FName("Blue")), Import.Asset->GetMaterialSlot(1));
	EXPECT_EQ(Import.Asset->GetMaterialSlot(2), nullptr);

	Durin::DMaterial* DefaultMaterial = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(MaterialPath, DefaultMaterial));
	ASSERT_TRUE(Durin::Asset::SavePackage(DefaultMaterial->GetPackage()));
	auto* SlotsProperty = static_cast<Durin::FArrayProperty*>(Import.Asset->GetClass()->FindPropertyByName("MaterialSlots"));
	ASSERT_NE(SlotsProperty, nullptr);
	auto* FirstSlot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(SlotsProperty->GetMutableElementPtr(Import.Asset, 0));
	ASSERT_NE(FirstSlot, nullptr);
	FirstSlot->DefaultMaterial = DefaultMaterial;
	Import.Asset->MarkPackageDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Import.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	ASSERT_EQ(Loaded->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->SlotId, OriginalIds[0]);
	EXPECT_EQ(Loaded->GetMaterialSlot(1)->SlotId, OriginalIds[1]);
	ASSERT_NE(Loaded->GetMaterialSlot(0)->DefaultMaterial.Get(), nullptr);
	EXPECT_EQ(Loaded->GetMaterialSlot(0)->DefaultMaterial->GetPackage()->GetPackagePath(), MaterialPath.ToString());
	EXPECT_FALSE(Loaded->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
}

TEST(FMaterialTests, StaticMeshMaterialSlotReconciliationPreservesOnlyUnambiguousIdentity)
{
	InitializeDObjectSystem();
	static const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshSlotReimport";
	static const bool bMountInitialized = [] {
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotReimport/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;
	const std::filesystem::path BaseSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";

	auto ImportBase = [&](std::string_view Name) -> Durin::DStaticMesh* {
		const std::string AssetPath = std::format("/StaticMeshSlotReimport/{}", Name);
		Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(BaseSource.generic_string(), AssetPath);
		EXPECT_TRUE(Import) << Import.Message;
		return Import.Asset;
	};
	auto Rebuild = [&](Durin::DStaticMesh* Mesh, std::string_view Name, std::string_view Materials,
		std::optional<std::pair<std::string_view, std::string_view>> Replacement = std::nullopt, bool LastOnly = false,
		std::optional<Durin::uint32> AppendedMaterialIndex = std::nullopt) {
		const std::filesystem::path SourcePath = Root / (std::string(Name) + ".gltf");
		WriteStaticMeshSlotVariant(SourcePath, Materials, Replacement, LastOnly, AppendedMaterialIndex);
		std::string Error;
		ASSERT_TRUE(Mesh->PostLoad(Error)) << Error;
	};

	Durin::DStaticMesh* Reordered = ImportBase("Reordered");
	ASSERT_NE(Reordered, nullptr);
	const Durin::FGuid RedId = Reordered->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid BlueId = Reordered->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Reordered, "Reordered", R"({ "name": "Blue" }, { "name": "Red" })");
	ASSERT_EQ(Reordered->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Reordered->GetMaterialSlot(0)->SlotId, BlueId);
	EXPECT_EQ(Reordered->GetMaterialSlot(1)->SlotId, RedId);

	Durin::DStaticMesh* RenameAndReorder = ImportBase("RenameAndReorder");
	ASSERT_NE(RenameAndReorder, nullptr);
	const Durin::FGuid OldRedId = RenameAndReorder->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid OldBlueId = RenameAndReorder->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(RenameAndReorder, "RenameAndReorder", R"({ "name": "Blue" }, { "name": "Crimson" })");
	EXPECT_EQ(RenameAndReorder->GetMaterialSlot(0)->SlotId, OldBlueId);
	EXPECT_NE(RenameAndReorder->GetMaterialSlot(1)->SlotId, OldRedId);
	EXPECT_NE(RenameAndReorder->GetMaterialSlot(1)->SlotId, OldBlueId);

	Durin::DStaticMesh* Added = ImportBase("Added");
	ASSERT_NE(Added, nullptr);
	const Durin::FGuid AddedRedId = Added->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid AddedBlueId = Added->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Added, "Added", R"({ "name": "Red" }, { "name": "Blue" }, { "name": "Green" })",
		std::nullopt, false, 2);
	ASSERT_EQ(Added->GetNumMaterialSlots(), 3u);
	EXPECT_EQ(Added->FindMaterialSlot(Durin::FName("Red"))->SlotId, AddedRedId);
	EXPECT_EQ(Added->FindMaterialSlot(Durin::FName("Blue"))->SlotId, AddedBlueId);
	EXPECT_TRUE(Added->FindMaterialSlot(Durin::FName("Green"))->SlotId.IsValid());

	Durin::DStaticMesh* Removed = ImportBase("Removed");
	ASSERT_NE(Removed, nullptr);
	const Durin::FGuid RemovedBlueId = Removed->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Removed, "Removed", R"({ "name": "Red" }, { "name": "Blue" })",
		std::pair<std::string_view, std::string_view>{R"("material": 0)", R"("material": 1)"});
	ASSERT_EQ(Removed->GetNumMaterialSlots(), 1u);
	EXPECT_EQ(Removed->GetMaterialSlot(0)->SlotId, RemovedBlueId);

	Durin::DStaticMesh* Duplicate = ImportBase("Duplicate");
	ASSERT_NE(Duplicate, nullptr);
	const Durin::FGuid DuplicateRedId = Duplicate->FindMaterialSlot(Durin::FName("Red"))->SlotId;
	const Durin::FGuid DuplicateBlueId = Duplicate->FindMaterialSlot(Durin::FName("Blue"))->SlotId;
	Rebuild(Duplicate, "Duplicate", R"({ "name": "Shared" }, { "name": "Shared" })");
	ASSERT_EQ(Duplicate->GetNumMaterialSlots(), 2u);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, DuplicateRedId);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, DuplicateBlueId);
	EXPECT_NE(Duplicate->GetMaterialSlot(1)->SlotId, DuplicateRedId);
	EXPECT_NE(Duplicate->GetMaterialSlot(1)->SlotId, DuplicateBlueId);
	EXPECT_NE(Duplicate->GetMaterialSlot(0)->SlotId, Duplicate->GetMaterialSlot(1)->SlotId);
}

TEST(FMaterialTests, VersionZeroStaticMeshMaterialSlotsMigrateDeterministically)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshSlotMigration";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotMigration/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath MeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotMigration/Mesh", MeshPath));
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult Import = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(Import) << Import.Message;
	auto* VersionProperty = Import.Asset->GetClass()->FindPropertyByName("MaterialSlotsVersion");
	auto* SlotsProperty = static_cast<Durin::FArrayProperty*>(Import.Asset->GetClass()->FindPropertyByName("MaterialSlots"));
	ASSERT_NE(VersionProperty, nullptr);
	ASSERT_NE(SlotsProperty, nullptr);
	*static_cast<Durin::uint32*>(VersionProperty->GetValuePtr(Import.Asset)) = 0;
	SlotsProperty->Resize(Import.Asset, 0);
	Import.Asset->MarkPackageDirty();
	ASSERT_TRUE(Durin::Asset::SavePackage(Import.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMesh* FirstLoad = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, FirstLoad));
	ASSERT_EQ(FirstLoad->GetNumMaterialSlots(), 2u);
	const std::array FirstIds{FirstLoad->GetMaterialSlot(0)->SlotId, FirstLoad->GetMaterialSlot(1)->SlotId};
	EXPECT_TRUE(FirstIds[0].IsValid());
	EXPECT_TRUE(FirstIds[1].IsValid());
	EXPECT_NE(FirstIds[0], FirstIds[1]);
	EXPECT_TRUE(FirstLoad->GetPackage()->IsDirty()) << "Version-zero slot migration must request an asset resave.";
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMesh* SecondLoad = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, SecondLoad));
	ASSERT_EQ(SecondLoad->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(SecondLoad->GetMaterialSlot(0)->SlotId, FirstIds[0]);
	EXPECT_EQ(SecondLoad->GetMaterialSlot(1)->SlotId, FirstIds[1]);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
}

TEST(FMaterialTests, StaticMeshImportSettingsValidateDistinctAxes)
{
	Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	EXPECT_TRUE(Settings.IsValid());
	Settings.RightAxis = Durin::EStaticMeshImportAxis::PositiveZ;
	std::string Error;
	EXPECT_FALSE(Settings.IsValid(&Error));
	EXPECT_FALSE(Error.empty());
}

TEST(FMaterialTests, StaticMeshImportSettingsPersistAcrossSourceRebuild)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshAxisImports";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/MeshAxisImportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const Durin::FStaticMeshImportSettings Settings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "AsymmetricAxes.obj";
	Durin::FStaticMeshImportResult ImportResult = Durin::DStaticMesh::ImportAsset(
		Source.generic_string(), "/MeshAxisImportTests/AsymmetricAxes", Settings);
	ASSERT_TRUE(ImportResult) << ImportResult.Message;
	ASSERT_NE(ImportResult.Asset, nullptr);
	EXPECT_EQ(ImportResult.Asset->GetImportSettings(), Settings);
	ASSERT_NE(ImportResult.Asset->GetRenderData(), nullptr);
	ASSERT_EQ(ImportResult.Asset->GetRenderData()->LODResources.size(), 1u);
	const std::vector<Durin::FVector3f> ImportedPositions = ImportResult.Asset->GetRenderData()->LODResources[0].Positions;

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MeshAxisImportTests/AsymmetricAxes", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DStaticMesh* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetImportSettings(), Settings);
	ASSERT_NE(Loaded->GetRenderData(), nullptr);
	ASSERT_EQ(Loaded->GetRenderData()->LODResources.size(), 1u);
	const auto& ReloadedPositions = Loaded->GetRenderData()->LODResources[0].Positions;
	ASSERT_EQ(ReloadedPositions.size(), ImportedPositions.size());
	for (size_t Index = 0; Index < ImportedPositions.size(); ++Index)
	{
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].x, ImportedPositions[Index].x);
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].y, ImportedPositions[Index].y);
		EXPECT_FLOAT_EQ(ReloadedPositions[Index].z, ImportedPositions[Index].z);
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FMaterialTests, LegacyStaticMeshComponentFixtureRoundTripsAfterMeshDependenciesLoad)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LegacyStaticMeshSlots";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/LegacyStaticMeshSlots/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath MeshPath;
	Durin::FAssetPath FirstMaterialPath;
	Durin::FAssetPath SecondMaterialPath;
	Durin::FAssetPath ComponentPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyStaticMeshSlots/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyStaticMeshSlots/First", FirstMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyStaticMeshSlots/Second", SecondMaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyStaticMeshSlots/Component", ComponentPath));

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	Durin::FStaticMeshImportResult MeshImport = Durin::DStaticMesh::ImportAsset(Source.generic_string(), MeshPath.ToString());
	ASSERT_TRUE(MeshImport) << MeshImport.Message;
	Durin::DMaterial* First = nullptr;
	Durin::DMaterial* Second = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(FirstMaterialPath, First));
	ASSERT_TRUE(Durin::Asset::CreateAsset(SecondMaterialPath, Second));
	ASSERT_TRUE(Durin::Asset::SavePackage(First->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Second->GetPackage()));

	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(ComponentPath, Component));
	Component->SetStaticMesh(MeshImport.Asset);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	ASSERT_TRUE(Durin::Asset::SavePackage(Component->GetPackage()));

	const auto* ComponentData = Durin::Asset::GetAssetRegistry().FindAsset(ComponentPath);
	ASSERT_NE(ComponentData, nullptr);
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, MeshPath), ComponentData->Dependencies.end());
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, FirstMaterialPath), ComponentData->Dependencies.end());
	EXPECT_NE(std::ranges::find(ComponentData->Dependencies, SecondMaterialPath), ComponentData->Dependencies.end());

	const std::filesystem::path FixturePath = std::filesystem::path(DURIN_TEST_WORK_DIR)
		/ "LegacyStaticMeshSlots" / "Component.dasset";
	std::vector<Durin::uint8> FixtureBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FixtureBytes, FixturePath.generic_string()));
	EXPECT_TRUE(ContainsSerializedField(FixtureBytes, "Material"));
	EXPECT_TRUE(ContainsSerializedField(FixtureBytes, "Materials"));
	EXPECT_FALSE(ContainsSerializedField(FixtureBytes, "MaterialSlotDataVersion"));
	EXPECT_FALSE(ContainsSerializedField(FixtureBytes, "MaterialOverrides"));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));

	Durin::DStaticMeshComponent* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ComponentPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	ASSERT_NE(Loaded->GetStaticMesh(), nullptr);
	ASSERT_NE(Loaded->GetStaticMesh()->GetRenderData(), nullptr);
	EXPECT_EQ(Loaded->GetNumMaterials(), 2u);
	ASSERT_NE(Loaded->GetMaterial(0), nullptr);
	ASSERT_NE(Loaded->GetMaterial(1), nullptr);
	EXPECT_EQ(Loaded->GetMaterial(0)->GetPackage()->GetPackagePath(), FirstMaterialPath.ToString());
	EXPECT_EQ(Loaded->GetMaterial(1)->GetPackage()->GetPackagePath(), SecondMaterialPath.ToString());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ComponentPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
}

TEST(FMaterialTests, MaterialInstanceAssetsRoundTripParentAndOverrides)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Materials";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/MaterialTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/Instance", InstancePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MaterialTests/BaseColorTexture", TexturePath));

	const std::filesystem::path TextureSource = std::filesystem::path(DURIN_TEST_WORK_DIR) / "MaterialBaseColor.png";
	WriteMaterialTextureFixture(TextureSource);
	Durin::FTexture2DImportResult TextureImport = Durin::DTexture2D::ImportAsset(TextureSource.generic_string(), TexturePath.ToString());
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	Base->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.6));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.35f);
	Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	const Durin::Asset::FAssetData* InstanceData =
		Durin::Asset::GetAssetRegistry().FindAsset(InstancePath);
	ASSERT_NE(InstanceData, nullptr);
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, BasePath), InstanceData->Dependencies.end());
	EXPECT_NE(std::ranges::find(InstanceData->Dependencies, TexturePath), InstanceData->Dependencies.end());
	ASSERT_FALSE(Instance->GetPackage()->IsDirty());
	const Durin::uint64 SavedVersion = Instance->GetRenderStateVersion();
	EXPECT_FALSE(Instance->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.2f));
	EXPECT_FALSE(Instance->SetVectorParameterValue(Durin::MaterialParameters::OpacityName(), Durin::FVector3(0.2)));
	EXPECT_FALSE(Instance->GetPackage()->IsDirty());
	EXPECT_EQ(Instance->GetRenderStateVersion(), SavedVersion);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));

	Durin::DMaterialInstance* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(InstancePath, Loaded));
	ASSERT_NE(Loaded->GetParent(), nullptr);
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.2f, 0.4f, 0.6f, 0.35f));
	Durin::DTexture2D* LoadedTexture = nullptr;
	ASSERT_TRUE(Loaded->GetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), LoadedTexture));
	ASSERT_NE(LoadedTexture, nullptr);
	EXPECT_EQ(Loaded->GetRenderData().BaseColorTexture, LoadedTexture->GetRenderResource());
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(Loaded->GetParent());
	ASSERT_NE(LoadedBase, nullptr);
	LoadedBase->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.6, 0.4, 0.2));
	ExpectColorNear(Loaded->GetRenderData().BaseColor, Durin::FVector4f(0.6f, 0.4f, 0.2f, 0.35f));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
}

TEST(FMaterialTests, LegacyParameterMapsAreSkippedWithoutMigration)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LegacyMaterials";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/LegacyMaterialTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath BasePath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyMaterialTests/Base", BasePath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/LegacyMaterialTests/Instance", InstancePath));

	Durin::DMaterial* Base = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(BasePath, Base));
	ASSERT_TRUE(Base->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3)));
	ASSERT_TRUE(Durin::Asset::SavePackage(Base->GetPackage()));

	Durin::DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(InstancePath, Instance));
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.25f));
	ASSERT_TRUE(Durin::Asset::SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));

	const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LegacyMaterials";
	std::vector<Durin::uint8> BaseBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BaseBytes, (Root / "Base.dasset").generic_string()));
	ASSERT_TRUE(RewriteSerializedFieldAsLegacyMap(BaseBytes, "ParameterDefinitions", "VectorParameters"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(BaseBytes)), Root / "Base.dasset"));

	std::vector<Durin::uint8> InstanceBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(InstanceBytes, (Root / "Instance.dasset").generic_string()));
	ASSERT_TRUE(RewriteSerializedFieldAsLegacyMap(InstanceBytes, "ParameterOverrides", "ScalarParameters"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(InstanceBytes)), Root / "Instance.dasset"));

	Durin::DMaterialInstance* LoadedInstance = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(InstancePath, LoadedInstance));
	ASSERT_NE(LoadedInstance, nullptr);
	auto* LoadedBase = Durin::Cast<Durin::DMaterial>(LoadedInstance->GetParent());
	ASSERT_NE(LoadedBase, nullptr);

	Durin::FVector3 BaseColor;
	ASSERT_TRUE(LoadedBase->GetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, Durin::FVector3(0.95, 0.62, 0.22));
	EXPECT_TRUE(LoadedInstance->GetParameterOverrides().empty());
	EXPECT_FALSE(LoadedInstance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));

	float Opacity = 0.0f;
	ASSERT_TRUE(LoadedInstance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(InstancePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BasePath));
}
