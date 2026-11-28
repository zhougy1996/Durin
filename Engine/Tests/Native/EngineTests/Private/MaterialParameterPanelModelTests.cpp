#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "Graph/MaterialEditorPreviewParameter.h"
#include "Graph/MaterialEditingOperations.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Materials/FunctionPortTestFixture.h"
#include "Materials/TypedMaterialGraphTestFixture.h"
#include "Materials/ExplicitMaterialProgramTestFixture.h"
#include "Materials/MaterialTestSupport.h"
#include "MaterialGraphOperations.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "EngineTestSupport.h"
#include "Asset/PackageReload.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Testing.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialParameterPanelModel.h"

#include <gtest/gtest.h>

namespace
{
	using FPropertyTransactionHarness = Durin::Tests::FTestEditorTransactorOwner;

	auto FindEntry(
		const Durin::MaterialEditor::FMaterialParameterPanelModel& Model,
		const Durin::FGuid& ParameterId
	) -> const Durin::MaterialEditor::FMaterialParameterPanelEntry*
	{
		const auto Entries = Model.GetEntries();
		const auto It = std::ranges::find(Entries, ParameterId, &Durin::MaterialEditor::FMaterialParameterPanelEntry::ParameterId);
		return It == Entries.end() ? nullptr : &*It;
	}

	auto MakeContext(
		FPropertyTransactionHarness& Transactions,
		std::string& Error
	) -> Durin::FPropertyViewContext
	{
		return {
			.ReportPropertyError = [&Error](std::string Message) { Error = std::move(Message); },
		};
	}

	auto MakeExpandedBase(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialParameterPanelModelTests, BuildsControlsAndResolvedSourceFromRuntimeSchema)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelSchemaBase");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.6f));

	const Durin::MaterialEditor::FMaterialParameterPanelModel Model(Child);
	const auto* BaseColor = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	const auto* Texture = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	const auto* Opacity = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	const auto* Roughness = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Roughness).Value);
	const auto* UVChannel = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVChannel);
	const auto* UVScale = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVScale);
	ASSERT_NE(BaseColor, nullptr);
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(Roughness, nullptr);
	ASSERT_NE(UVChannel, nullptr);
	ASSERT_NE(UVScale, nullptr);
	EXPECT_EQ(BaseColor->Control, Durin::MaterialEditor::EMaterialParameterControlKind::Color);
	EXPECT_EQ(Texture->Control, Durin::MaterialEditor::EMaterialParameterControlKind::AssetPicker);
	EXPECT_EQ(Opacity->Control, Durin::MaterialEditor::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(Roughness->Control, Durin::MaterialEditor::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(UVChannel->Control, Durin::MaterialEditor::EMaterialParameterControlKind::IntegerScalar);
	EXPECT_EQ(UVChannel->Definition->GroupName.ToString(), "Surface/Base");
	EXPECT_EQ(UVScale->Control, Durin::MaterialEditor::EMaterialParameterControlKind::Vector);
	EXPECT_EQ(UVScale->Definition->Type, Durin::EMaterialParameterType::Vector4);
	EXPECT_EQ(Opacity->Source, Parent);
	EXPECT_FLOAT_EQ(Opacity->Value.GetScalar(), 0.6f);
	EXPECT_FALSE(Opacity->bHasLocalOverride);
	EXPECT_TRUE(Opacity->bCanOverride);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, IntegerPresentationCanonicalizesSubmittedValues)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelIntegerMaterial");
	auto* Material = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelIntegerInstance");
	ASSERT_TRUE(Material->SetParent(Base));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 0));
	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	const Durin::MaterialEditor::FMaterialParameterPanelModel Model(Material);
	const auto* UVChannel = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVChannel);
	ASSERT_NE(UVChannel, nullptr);

	auto Value = UVChannel->Value;
	Value.GetScalar() = 2.6f;
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *UVChannel, Value, false));
	float StoredValue = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), StoredValue));
	EXPECT_FLOAT_EQ(StoredValue, 3.0f);

	Value.GetScalar() = -10.0f;
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *UVChannel, Value, false));
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), StoredValue));
	EXPECT_FLOAT_EQ(StoredValue, 0.0f);
	EXPECT_TRUE(Error.empty());

	Transactions->Reset();
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, GuidRootEditsSurviveIndexChangesAndCoalesceOrCancel)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelEditBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelEditInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4)));

	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::MaterialEditor::FMaterialParameterPanelModel InitialModel(Instance);
	const auto* InitialOpacity = FindEntry(InitialModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(InitialOpacity, nullptr);
	ASSERT_TRUE(InitialModel.SetOverrideEnabled(PropertyView, Context, *InitialOpacity, true));
	ASSERT_TRUE(Instance->HasLocalParameterValueByInfo({.Name = Durin::AssetForge::Builtins::MaterialParameters::OpacityName()}));

	Durin::MaterialEditor::FMaterialParameterPanelModel EditModel(Instance);
	const auto* Opacity = FindEntry(EditModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_TRUE(Opacity->bHasLocalOverride);
	// Removing an earlier entry changes Opacity's array index. The retained panel row
	// remains valid because every scratch mutation resolves the entry by GUID.
	ASSERT_TRUE(Instance->ClearParameterValueByInfo({.Name = Durin::AssetForge::Builtins::MaterialParameters::BaseColorName()}));

	auto FirstValue = Opacity->Value;
	FirstValue.GetScalar() = 0.4f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FirstValue, true));
	auto FinalValue = Opacity->Value;
	FinalValue.GetScalar() = 0.25f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FinalValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, false));
	float Value = 0.0f;
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 1.0f);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	Durin::MaterialEditor::FMaterialParameterPanelModel CancelModel(Instance);
	const auto* CancelOpacity = FindEntry(CancelModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(CancelOpacity, nullptr);
	auto CancelValue = CancelOpacity->Value;
	CancelValue.GetScalar() = 0.8f;
	ASSERT_TRUE(CancelModel.SubmitValueEdit(PropertyView, Context, *CancelOpacity, CancelValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);
	EXPECT_TRUE(Error.empty());

	Transactions->Reset();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, ResetAndOrphanRemovalAreTransactional)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelOrphanBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelOrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto Name = Durin::AssetForge::Builtins::MaterialParameters::OpacityName();
	ASSERT_TRUE(Base->SetScalarParameterValue(Name, 0.65f));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.3f));

	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::MaterialEditor::FMaterialParameterPanelModel OverrideModel(Instance);
	const auto* Opacity = FindEntry(OverrideModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(Opacity, nullptr);
	const auto ExpectOpacity = [&](float Value, const Durin::DMaterialInterface* Source) {
		OverrideModel.Refresh();
		const auto* Row = FindEntry(OverrideModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
			Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
		ASSERT_NE(Row, nullptr);
		EXPECT_EQ(Row->Source, Source);
		EXPECT_FLOAT_EQ(Row->Value.GetScalar(), Value);
	};
	ASSERT_TRUE(OverrideModel.SetOverrideEnabled(PropertyView, Context, *Opacity, false));
	ExpectOpacity(0.65f, Base);
	ASSERT_TRUE(Transactions->Undo());
	ExpectOpacity(0.3f, Instance);
	ASSERT_TRUE(Transactions->Redo());
	ExpectOpacity(0.65f, Base);

	ASSERT_TRUE(Instance->SetScalarParameterValue(Name, 0.3f));
	ASSERT_TRUE(Instance->SetParent(nullptr));
	Durin::MaterialEditor::FMaterialParameterPanelModel OrphanModel(Instance);
	ASSERT_EQ(OrphanModel.GetEntries().size(), 1u);
	const auto& Orphan = OrphanModel.GetEntries().front();
	EXPECT_TRUE(Orphan.bOrphan);
	EXPECT_EQ(Orphan.ParameterId, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_TRUE(OrphanModel.RemoveOrphan(PropertyView, Context, Orphan));
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Instance->IsParameterValueOrphanByInfo({.Name = Durin::AssetForge::Builtins::MaterialParameters::OpacityName()}));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);
	EXPECT_TRUE(Error.empty());

	auto* Property = Instance->GetClass()->FindPropertyByName(Durin::FMaterialScalarParameterValue::PropertyName());
	ASSERT_NE(Property, nullptr);
	auto* Records = Property->ContainerPtrToValuePtr<std::vector<Durin::FMaterialScalarParameterValue>>(Instance);
	Records->push_back({.Value = 1.f, .ParameterInfo = {.Name = "FirstOrphan"}});
	Records->push_back({.Value = 2.f, .ParameterInfo = {.Name = "SecondOrphan"}});
	const auto LegacyId = Durin::FGuid::NewGuid();
	Records->push_back({.ParameterId = LegacyId, .Value = 3.f});
	Durin::MaterialEditor::FMaterialParameterPanelModel NamedOrphans(Instance);
	ASSERT_EQ(NamedOrphans.GetEntries().size(), 3u);
	ASSERT_TRUE(NamedOrphans.RemoveOrphan(PropertyView, Context, *FindEntry(NamedOrphans, LegacyId)));
	ASSERT_EQ(Records->size(), 2u);
	EXPECT_TRUE(Instance->HasLocalParameterValueByInfo({.Name = "FirstOrphan"}));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_EQ(Records->size(), 3u);
	EXPECT_EQ(Records->back().ParameterId, LegacyId);
	ASSERT_TRUE(NamedOrphans.RemoveOrphan(PropertyView, Context, NamedOrphans.GetEntries()[1]));
	EXPECT_TRUE(Instance->HasLocalParameterValueByInfo({.Name = "FirstOrphan"}));
	EXPECT_FALSE(Instance->HasLocalParameterValueByInfo({.Name = "SecondOrphan"}));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Instance->HasLocalParameterValueByInfo({.Name = "SecondOrphan"}));

	Transactions->Reset();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, BaseAndTexturePickerValuesUseSharedUndoHistory)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelValueBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelValueInstance");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "PanelValueTexture");
	ASSERT_TRUE(Instance->SetParent(Base));

	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::MaterialEditor::FMaterialParameterPanelModel BaseModel(Base);
	const auto* BaseOpacity = FindEntry(BaseModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(BaseOpacity, nullptr);
	Durin::MaterialEditor::FMaterialEditorPreviewParameters Parameters(Base);
	const auto Preview = Parameters.Find(BaseOpacity->ParameterId);
	ASSERT_TRUE(Preview);
	Durin::PropertyEditor::FReflectedPropertyHandle Handle(Preview->GetEditTarget());
	ASSERT_EQ(Handle.SetValue(.7f), Durin::PropertyEditor::FPropertyAccess::Success);
	float Opacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);

	Durin::MaterialEditor::FMaterialParameterPanelModel InheritedModel(Instance);
	const auto* InheritedTexture = FindEntry(InheritedModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	ASSERT_NE(InheritedTexture, nullptr);
	ASSERT_TRUE(InheritedModel.SetOverrideEnabled(PropertyView, Context, *InheritedTexture, true));
	Durin::MaterialEditor::FMaterialParameterPanelModel TextureModel(Instance);
	const auto* TextureEntry = FindEntry(TextureModel, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	ASSERT_NE(TextureEntry, nullptr);
	auto TextureValue = TextureEntry->Value;
	TextureValue.GetTexture().Texture = Texture;
	TextureValue.GetTexture().SamplerState.AddressU = Durin::EMaterialSamplerAddressMode::ClampToEdge;
	TextureValue.GetTexture().TextureFallback = Durin::EMaterialTextureFallback::Black;
	ASSERT_TRUE(TextureModel.SubmitValueEdit(PropertyView, Context, *TextureEntry, TextureValue, false));
	Durin::DTexture* ResolvedTexture = nullptr;
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	Durin::FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValueByInfo(TextureEntry->ParameterInfo, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, nullptr);
	ASSERT_TRUE(Instance->ResolveParameterValueByInfo(TextureEntry->ParameterInfo, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState.AddressU, Durin::EMaterialSamplerAddressMode::Repeat);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::White);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	ASSERT_TRUE(Instance->ResolveParameterValueByInfo(TextureEntry->ParameterInfo, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	ASSERT_TRUE(Instance->SetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), nullptr));
	ASSERT_TRUE(Instance->ResolveParameterValueByInfo(TextureEntry->ParameterInfo, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	TextureValue.GetTexture().SamplerState.AddressU = static_cast<Durin::EMaterialSamplerAddressMode>(255);
	const auto InvalidSampling = Instance->SetParameterValueByInfo(TextureEntry->ParameterInfo, TextureValue);
	ASSERT_FALSE(InvalidSampling);
	EXPECT_NE(InvalidSampling.error().find(TextureEntry->ParameterId.ToString()), std::string::npos);
	EXPECT_FALSE(TextureModel.SubmitValueEdit(PropertyView, Context, *TextureEntry, TextureValue, false));
	EXPECT_TRUE(Error.empty());

	Transactions->Reset();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, ColorPopupCoalescesReflectedOverridesAndCancels)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	using namespace MonaImGui::PropertyEdit;
	using namespace AssetForge::Builtins::MaterialParameters;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(MakeExpandedBase("ColorPopupBase"));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "ColorPopupInstance"));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	ASSERT_TRUE(Instance->SetVectorParameterValue(BaseColorName(), FVector3(0.25, 0.5, 0.75)));
	const auto Id = GetBuiltinParameterIds(EMaterialBuiltinParameterRole::BaseColor).Value;
	const auto Read = [&] {
		FMaterialParameterValue Value;
		EXPECT_TRUE(Instance->GetLocalParameterValueByInfo({.Name = BaseColorName()}, Value));
		return Value;
	};
	const auto Original = Read();
	const auto First = FMaterialParameterValue::MakeVector4({0.5, 0.5, 0.75, 1});
	const auto Final = FMaterialParameterValue::MakeVector4({0.75, 0.5, 0.75, 1});
	FPropertyTransactionHarness Transactions;
	FPropertyView View;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.ConfigInputTrickleEventQueue = false;
	IO.DisplaySize = {900, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	const auto Frame = [&](const FMaterialParameterValue* Edited = nullptr, bool bOpen = false, bool bClose = false) {
		ImGui::NewFrame();
		ImGui::Begin("Instance color parameter");
		const ImGuiID PopupId = ImGui::GetID("##Picker");
		if (bOpen) ImGui::OpenPopup("##Picker");
		const bool bIsOpen = ImGui::BeginPopup("##Picker");
		if (bIsOpen)
		{
			if (bClose) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		const auto Picker = FPropertyInteractionState::FromPopup(PopupId, bIsOpen);
		const FMaterialParameterPanelModel Model(Instance.Get());
		const auto* Entry = FindEntry(Model, Id);
		ASSERT_NE(Entry, nullptr);
		EXPECT_TRUE(Model.UpdateValueEdit(View, Context, *Entry, Picker, Edited));
		EXPECT_TRUE(View.EndInteractionFrame(Context));
		ImGui::End(); ImGui::Render();
	};
	Frame(nullptr, true);
	EXPECT_FALSE(View.IsEditing());
	Frame(&First);
	EXPECT_EQ(Read(), First);
	Frame();
	EXPECT_TRUE(View.IsEditing());
	Frame(&Final);
	EXPECT_EQ(Read(), Final);
	Frame(nullptr, false, true);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Read(), Original);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Read(), Final);
	Transactions->Reset();
	Frame(nullptr, true);
	Frame(&First);
	EXPECT_EQ(Read(), First);
	IO.AddKeyEvent(ImGuiKey_Escape, true); Frame();
	EXPECT_EQ(Read(), Final);
	EXPECT_FALSE(View.IsEditing());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_TRUE(Error.empty()) << Error;
}

TEST(FMaterialParameterPanelModelTests, ContinuousScalarAndVectorUseGestureBoundariesAndCancelMissingControls)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	using namespace MonaImGui::PropertyEdit;
	using namespace AssetForge::Builtins::MaterialParameters;
	InitializeDObjectSystem();
	for (const auto Role : {EMaterialBuiltinParameterRole::Opacity, EMaterialBuiltinParameterRole::BaseColor})
	{
		TStrongObjectPtr<DMaterial> Base(MakeExpandedBase("ContinuousBase"));
		TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "ContinuousInstance"));
		ASSERT_TRUE(Instance->SetParent(Base.Get()));
		FPropertyTransactionHarness Transactions;
		FPropertyView View;
		std::string Error;
		const auto Context = MakeContext(Transactions, Error);
		const auto Id = GetBuiltinParameterIds(Role).Value;
		FMaterialParameterPanelModel Model(Instance.Get());
		ASSERT_TRUE(Model.SetOverrideEnabled(View, Context, *FindEntry(Model, Id), true));
		Transactions->Reset();
		const auto Read = [&] {
			FMaterialParameterValue Value;
			EXPECT_TRUE(Instance->GetLocalParameterValueByInfo(FindEntry(Model, Id)->ParameterInfo, Value));
			return Value;
		};
		const auto Original = Read();
		const auto Edited = Role == EMaterialBuiltinParameterRole::Opacity
			? FMaterialParameterValue::MakeScalar(.25f) : FMaterialParameterValue::MakeVector4({.25, .5, .75, 1});
		const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.ConfigInputTrickleEventQueue = false;
		IO.DisplaySize = {900, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		const auto Frame = [&](bool bChange, bool bActive = true, bool bVisible = true) {
			ImGui::NewFrame(); ImGui::Begin("Continuous instance");
			if (bVisible)
			{
				const FMaterialParameterPanelModel Current(Instance.Get());
				const auto* Entry = FindEntry(Current, Id);
				const auto State = FPropertyInteractionState::FromWidget(ImGui::GetID("Value"), {bActive, !bActive}, false, bChange);
				EXPECT_TRUE(Current.UpdateValueEdit(View, Context, *Entry, State, bChange ? &Edited : nullptr));
			}
			EXPECT_TRUE(View.EndInteractionFrame(Context));
			ImGui::End(); ImGui::Render();
		};
		Frame(true); Frame(false);
		EXPECT_EQ(Read(), Edited);
		EXPECT_TRUE(View.IsEditing());
		Frame(false, false);
		EXPECT_FALSE(View.IsEditing());
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Read(), Original);
		EXPECT_FALSE(Transactions->CanUndo());
		Transactions->Reset();
		Frame(true);
		IO.AddKeyEvent(ImGuiKey_Escape, true); Frame(true, false);
		EXPECT_EQ(Read(), Original);
		IO.AddKeyEvent(ImGuiKey_Escape, false); Frame(true);
		Frame(false, false, false);
		EXPECT_EQ(Read(), Original);
		EXPECT_FALSE(View.IsEditing());
		EXPECT_FALSE(Transactions->CanUndo());
		EXPECT_TRUE(Error.empty()) << Error;
	}
}

TEST(FMaterialParameterPanelModelTests, SwitchingOverrideCommitsEachParameterSeparately)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	using namespace AssetForge::Builtins::MaterialParameters;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(MakeExpandedBase("SwitchOverridesBase"));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "SwitchOverridesInstance"));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	FPropertyTransactionHarness Transactions;
	FPropertyView View;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	const auto Alpha = GetBuiltinParameterIds(EMaterialBuiltinParameterRole::Opacity).Value;
	const auto Beta = GetBuiltinParameterIds(EMaterialBuiltinParameterRole::Roughness).Value;
	FMaterialParameterPanelModel Model(Instance.Get());
	ASSERT_TRUE(Model.SetOverrideEnabled(View, Context, *FindEntry(Model, Alpha), true));
	ASSERT_TRUE(Model.SetOverrideEnabled(View, Context, *FindEntry(Model, Beta), true));
	Model.Refresh();
	Transactions->Reset();
	const auto Read = [&](const FGuid& Id) {
		FMaterialParameterValue Value;
		EXPECT_TRUE(Instance->GetLocalParameterValueByInfo(FindEntry(Model, Id)->ParameterInfo, Value));
		return Value;
	};
	const auto BeforeAlpha = Read(Alpha), BeforeBeta = Read(Beta);
	const auto Edited = FMaterialParameterValue::MakeScalar(.123f);
	ASSERT_TRUE(Model.SubmitValueEdit(View, Context, *FindEntry(Model, Alpha), Edited, true)) << Error;
	ASSERT_TRUE(Model.SubmitValueEdit(View, Context, *FindEntry(Model, Beta), Edited, false));
	EXPECT_EQ(Read(Alpha), Edited);
	EXPECT_EQ(Read(Beta), Edited);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Read(Alpha), Edited);
	EXPECT_EQ(Read(Beta), BeforeBeta);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Read(Alpha), BeforeAlpha);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Read(Alpha), Edited);
	EXPECT_EQ(Read(Beta), Edited);
}

TEST(FMaterialParameterPanelModelTests, GraphDefaultSessionsRemainParameterScoped)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelIdentityBase");
	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::MaterialEditor::FMaterialParameterPanelModel Model(Base);
	const auto* Opacity = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	const auto* BaseColor = FindEntry(Model, Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(BaseColor, nullptr);

	auto OpacityValue = Opacity->Value;
	OpacityValue.GetScalar() = 0.55f;
	Durin::MaterialEditor::FMaterialEditorPreviewParameters Parameters(Base);
	auto OpacityPreview = Parameters.Find(Opacity->ParameterId);
	auto ColorPreview = Parameters.Find(BaseColor->ParameterId);
	ASSERT_TRUE(OpacityPreview);
	ASSERT_TRUE(ColorPreview);
	Durin::PropertyEditor::FPropertyEdit OpacitySession, ColorSession;
	ASSERT_TRUE(OpacitySession.Begin(OpacityPreview->GetEditTarget(), "Edit parameter", *Transactions.Get()));
	ASSERT_TRUE(OpacitySession.SetValue(OpacityValue.GetScalar()));
	ASSERT_TRUE(OpacitySession.Commit());
	auto ColorValue = BaseColor->Value;
	ColorValue.GetVector4() = Durin::FVector4(0.1, 0.2, 0.3, 0);
	ASSERT_TRUE(ColorSession.Begin(ColorPreview->GetEditTarget(), "Edit parameter", *Transactions.Get()));
	ASSERT_TRUE(ColorSession.SetValue(ColorValue.GetVector4()));
	// Switching logical GUIDs commits the first continuous edit. Cancelling the
	// second must not restore the entire collection to the first edit's origin.
	ASSERT_TRUE(ColorSession.Cancel());

	float ResolvedOpacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 0.55f);
	Durin::FVector3 ResolvedColor;
	ASSERT_TRUE(Base->GetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), ResolvedColor));
	EXPECT_EQ(ResolvedColor, Durin::FVector3(BaseColor->Value.GetVector4()));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 1.0f);
	EXPECT_TRUE(Error.empty());

	Transactions->Reset();
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}


TEST(FMaterialParameterPanelModelTests, RowsFollowParentOverridesAndGraphChanges)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelCacheBase");
	auto* OtherBase = MakeExpandedBase("PanelCacheOtherBase");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelCacheParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelCacheChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	const auto Id = Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
		Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	Durin::MaterialEditor::FMaterialParameterPanelModel Model(Child);
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.4f));
	EXPECT_TRUE(Model.NeedsRefresh());
	Model.Refresh();
	ASSERT_NE(FindEntry(Model, Id), nullptr);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.4f);
	EXPECT_EQ(FindEntry(Model, Id)->Source, Parent);
	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), 0.2f));
	Model.Refresh();
	EXPECT_TRUE(FindEntry(Model, Id)->bHasLocalOverride);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.2f);

	ASSERT_TRUE(Base->SetMaterialExpressions({}, {}));
	Model.Refresh();
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	EXPECT_TRUE(Model.GetEntries().front().bOrphan);
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Base));
	Model.Refresh();
	EXPECT_FALSE(FindEntry(Model, Id)->bOrphan);
	ASSERT_TRUE(Parent->SetParent(OtherBase));
	Model.Refresh();
	ASSERT_NE(FindEntry(Model, Id), nullptr);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.2f);
	ASSERT_TRUE(Parent->SetParent(nullptr));
	Model.Refresh();
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	EXPECT_TRUE(Model.GetEntries().front().bOrphan);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OtherBase);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, ReflectedDefaultEditsUpdateInheritedValuesAndUndo)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelDefaultCacheBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelDefaultCacheInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto Id = Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
		Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	Durin::MaterialEditor::FMaterialParameterPanelModel BaseModel(Base);
	Durin::MaterialEditor::FMaterialParameterPanelModel Model(Instance);
	FPropertyTransactionHarness Transactions;
	Durin::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	ASSERT_NE(FindEntry(BaseModel, Id), nullptr);
	const auto Target = MakeMaterialValueTarget(Base, Id, "ScalarValue");
	ASSERT_TRUE(Target.has_value());
	ASSERT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* Property, void* Container, uint32 Index) {
			*Property->ContainerPtrToValuePtr<float>(Container, Index) = 0.35f;
		}, true));
	Model.Refresh();
	BaseModel.Refresh();
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.35f);
	EXPECT_FLOAT_EQ(FindEntry(BaseModel, Id)->Definition->Value.GetScalar(), 0.35f);
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, false));
	ASSERT_TRUE(Transactions->Undo());
	Model.Refresh();
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 1.0f);
	EXPECT_TRUE(Error.empty());
	Transactions->Reset();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, RootPanelIncludesUnreachableCustomDefaults)
{
	InitializeDObjectSystem();
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "UnreachableDefaults");
	Durin::FMaterialParameterDefinition Definition;
	Definition.Id = Durin::FGuid::NewGuid();
	Definition.Name = "UnusedTint";
	Definition.Type = Durin::EMaterialParameterType::Vector4;
	Definition.Value = Durin::FMaterialParameterValue::MakeVector4({1, 2, 3, 4});
	Durin::Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Definition};
	Graph.Add(Durin::EMaterialProgramOpcode::Parameter, Durin::EMaterialProgramValueType::Float4, {}, Definition.Id, {}, Definitions);
	ASSERT_TRUE(Graph.Apply(*Material));
	const Durin::MaterialEditor::FMaterialParameterPanelModel Model(Material);
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	const auto& Entry = Model.GetEntries().front();
	EXPECT_EQ(Entry.ParameterId, Definition.Id);
	EXPECT_EQ(Entry.Control, Durin::MaterialEditor::EMaterialParameterControlKind::Vector);
	EXPECT_EQ(Entry.Value.GetVector4(), Durin::FVector4(1, 2, 3, 4));
	EXPECT_FALSE(Entry.bOrphan);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialParameterPanelModelTests, TypedResourceOutputsSkipUnusedUVDependencies)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	Base->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto UV = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	UV->Metadata.Id = FGuid::NewGuid(); UV->Metadata.Name = "OnlySampleUV";
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Sample->Metadata.Id = FGuid::NewGuid(); Sample->Metadata.Name = "SharedTexture";
	Sample->TextureUsage = ETextureUsage::Normal;
	auto Mask = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>();
	Mask->Input = {UV.Get()}; Mask->Components = {0, 1};
	Sample->UV = {Mask.Get()};
	auto ResourceConsumer = Testing::MakeGraphExpression<DMaterialExpressionTextureSample2D>();
	ResourceConsumer->Texture = {Sample.Get(), 7};
	const std::array<DMaterialExpression*, 4> Expressions{UV.Get(), Mask.Get(), Sample.Get(), ResourceConsumer.Get()};
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_OpacityMask) = {ResourceConsumer.Get(), 5};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	MaterialEditor::FMaterialParameterPanelModel Model(Instance.Get());
	EXPECT_NE(FindEntry(Model, Sample->Metadata.Id), nullptr);
	EXPECT_EQ(FindEntry(Model, UV->Metadata.Id), nullptr);
	Outputs.Get(EMaterialProperty::MP_Normal) = {Sample.Get(), 1};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_NE(FindEntry(Model, UV->Metadata.Id), nullptr);
	Outputs.Get(EMaterialProperty::MP_Normal).Connection = {};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_EQ(FindEntry(Model, UV->Metadata.Id), nullptr);
	EXPECT_NE(FindEntry(Model, Sample->Metadata.Id), nullptr);
}

TEST(FMaterialParameterPanelModelTests, ReachabilitySharesFunctionOutputAnalysisAndTracksTransitiveEdits)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Sibling(NewObject<DMaterialInstance>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, NAME_None));
	Base->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto A = FGuid::NewGuid(), B = FGuid::NewGuid();
	const auto X = FGuid::NewGuid(), Y = FGuid::NewGuid();
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {{.Id = A, .Type = EMaterialProgramValueType::Float, .Name = "A", .bRequired = true},
		{.Id = B, .Type = EMaterialProgramValueType::Float, .Name = "B", .bRequired = true}};
	Signature.Outputs = {{.Id = X, .Type = EMaterialProgramValueType::Float, .Name = "X"},
		{.Id = Y, .Type = EMaterialProgramValueType::Float, .Name = "Y"}};
	auto InputA = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(); InputA->Port.Id = A;
	auto InputB = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(); InputB->Port.Id = B;
	auto OutputX = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>(); OutputX->Port.Id = X;
	auto OutputY = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>(); OutputY->Port.Id = Y;
	OutputX->Source = {InputA.Get()}; OutputY->Source = {InputB.Get()};
	const std::array<DMaterialExpression*, 4> Body{InputA.Get(), InputB.Get(), OutputX.Get(), OutputY.Get()};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	auto Nested = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>();
	Nested->Function = Function.Get();
	Nested->FunctionInputs = {{A, EMaterialProgramValueType::Float, {InputA.Get()}},
		{B, EMaterialProgramValueType::Float, {InputB.Get()}}};
	Nested->FunctionOutputs = {{X, EMaterialProgramValueType::Float}, {Y, EMaterialProgramValueType::Float}};
	OutputX->Source = {Nested.Get(), 0, X}; OutputY->Source = {Nested.Get(), 0, Y};
	const std::array<DMaterialExpression*, 5> WrapperBody{InputA.Get(), InputB.Get(), OutputX.Get(), OutputY.Get(), Nested.Get()};
	ASSERT_TRUE(Wrapper->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, WrapperBody)));
	auto ParameterA = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	ParameterA->Metadata.Id = FGuid::NewGuid(); ParameterA->Metadata.Name = "ParameterA";
	auto ParameterB = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	ParameterB->Metadata.Id = FGuid::NewGuid(); ParameterB->Metadata.Name = "ParameterB";
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>();
	Call->Function = Wrapper.Get();
	Call->FunctionInputs = {{A, EMaterialProgramValueType::Float, {ParameterA.Get()}},
		{B, EMaterialProgramValueType::Float, {ParameterB.Get()}}};
	Call->FunctionOutputs = Nested->FunctionOutputs;
	const std::array<DMaterialExpression*, 3> Expressions{ParameterA.Get(), ParameterB.Get(), Call.Get()};
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_Roughness) = {Call.Get(), 0, X};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	ASSERT_TRUE(Sibling->SetParent(Base.Get()));
	const auto Initial = Instance->GetParameterReachability();
	ASSERT_TRUE(Initial->Validation);
	EXPECT_EQ(Initial, Base->GetParameterReachability());
	EXPECT_EQ(Initial, Sibling->GetParameterReachability());
	EXPECT_TRUE(Initial->ParameterNames.contains(ParameterA->Metadata.Name));
	EXPECT_FALSE(Initial->ParameterNames.contains(ParameterB->Metadata.Name));
	MaterialEditor::FMaterialParameterPanelModel Model(Instance.Get());
	EXPECT_NE(FindEntry(Model, ParameterA->Metadata.Id), nullptr);
	EXPECT_EQ(FindEntry(Model, ParameterB->Metadata.Id), nullptr);
	ASSERT_TRUE(Instance->SetParameterValueByInfo({.Name = ParameterA->Metadata.Name}, FMaterialParameterValue::MakeScalar(0.4f)));
	const auto Unreachable = Instance->SetParameterValueByInfo({.Name = ParameterB->Metadata.Name}, FMaterialParameterValue::MakeScalar(0.4f));
	ASSERT_FALSE(Unreachable);
	EXPECT_NE(Unreachable.error().find(ParameterB->Metadata.Id.ToString()), std::string::npos);
	EXPECT_EQ(Initial, Instance->GetParameterReachability());
	EXPECT_FALSE(Model.Refresh());

	// Only the nested function changes; neither the root nor wrapper revision advances.
	const auto RootRevision = Base->GetMaterialProgramRevision();
	const auto WrapperRevision = Wrapper->GetFunctionRevision();
	OutputX->Source = {InputB.Get()}; OutputY->Source = {InputA.Get()};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	EXPECT_EQ(RootRevision, Base->GetMaterialProgramRevision());
	EXPECT_EQ(WrapperRevision, Wrapper->GetFunctionRevision());
	const auto Changed = Instance->GetParameterReachability();
	ASSERT_TRUE(Changed->Validation);
	EXPECT_NE(Initial, Changed);
	EXPECT_FALSE(Changed->ParameterNames.contains(ParameterA->Metadata.Name));
	EXPECT_TRUE(Changed->ParameterNames.contains(ParameterB->Metadata.Name));
	EXPECT_TRUE(Initial->ParameterNames.contains(ParameterA->Metadata.Name));
	EXPECT_TRUE(Instance->IsParameterValueOrphanByInfo({.Name = ParameterA->Metadata.Name}));
	EXPECT_TRUE(Model.NeedsRefresh());
	EXPECT_TRUE(Model.Refresh());
	ASSERT_NE(FindEntry(Model, ParameterA->Metadata.Id), nullptr);
	EXPECT_TRUE(FindEntry(Model, ParameterA->Metadata.Id)->bOrphan);
	EXPECT_NE(FindEntry(Model, ParameterB->Metadata.Id), nullptr);

	// Removing a used output disconnects the wrapper. Recreating the port requires an explicit reconnection.
	auto BrokenSignature = Signature;
	BrokenSignature.Outputs[0].Id = FGuid::NewGuid();
	OutputX->Port.Id = BrokenSignature.Outputs[0].Id;
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(BrokenSignature, Body)));
	const auto Failed = Instance->GetParameterReachability();
	EXPECT_FALSE(Failed->Validation);
	EXPECT_TRUE(Failed->ParameterNames.empty());
	EXPECT_FALSE(Failed->Validation.Diagnostics.empty());
	OutputX->Port.Id = X;
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	EXPECT_FALSE(Instance->GetParameterReachability()->Validation);
	OutputX->Source = {Nested.Get(), 0, X};
	OutputY->Source = {Nested.Get(), 0, Y};
	ASSERT_TRUE(Wrapper->SetFunctionExpressions(Testing::WithFunctionPorts(Signature, WrapperBody)));
	const auto Recovered = Instance->GetParameterReachability();
	ASSERT_TRUE(Recovered->Validation);
	EXPECT_TRUE(Recovered->ParameterNames.contains(ParameterB->Metadata.Name));
	EXPECT_EQ(Recovered, Sibling->GetParameterReachability());
}

TEST(FMaterialParameterPanelModelTests, MetadataNotificationsReachEachObserverAcrossUndoRedo)
{
	using namespace Durin;
	using namespace Durin::MaterialEditor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(MakeExpandedBase("PanelMetadataNotifications"));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "PanelMetadataInstance"));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	const auto Id = Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	FMaterialParameterPanelModel First(Base.Get()), Second(Instance.Get());
	Tests::FTestEditorTransactorOwner Transactions;
	ASSERT_TRUE(MaterialEditing::RenameParameter(*Base, Id, "Coverage"));
	EXPECT_TRUE(First.NeedsRefresh());
	EXPECT_TRUE(Second.NeedsRefresh());
	First.Refresh();
	EXPECT_FALSE(First.NeedsRefresh());
	EXPECT_TRUE(Second.NeedsRefresh());
	Second.Refresh();
	ASSERT_NE(FindEntry(Second, Id), nullptr);
	EXPECT_EQ(FindEntry(Second, Id)->Definition->Name, FName("Coverage"));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Second.NeedsRefresh());
	Second.Refresh();
	EXPECT_EQ(FindEntry(Second, Id)->Definition->Name, Durin::AssetForge::Builtins::MaterialParameters::OpacityName());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Second.NeedsRefresh());
	Second.Refresh();
	EXPECT_EQ(FindEntry(Second, Id)->Definition->Name, FName("Coverage"));
	Transactions->Reset();
}

TEST(FMaterialParameterPanelModelTests, ParentPackageReplacementInvalidatesRetainedRows)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("ParameterPanelReload");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/ParameterPanelReload/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ParameterPanelReload/Base", Path));
	DMaterial* Base = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Base));
	// Parent replacement needs one retained row, not the full expanded PBR recipe.
	const auto Id = Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	auto Opacity = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Opacity->Metadata.Id = Id; Opacity->Metadata.Name = Durin::AssetForge::Builtins::MaterialParameters::OpacityName();
	Opacity->DefaultValue = 1.f;
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_Opacity) = {Opacity.Get()};
	ASSERT_TRUE(Base->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Opacity.Get()}, Outputs));
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "ReloadedParentInstance"));
	ASSERT_TRUE(Instance->SetParent(Base));
	MaterialEditor::FMaterialParameterPanelModel Model(Instance.Get());
	ASSERT_TRUE(Base->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::OpacityName(), .25f));
	Model.Refresh();
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), .25f);
	auto Reload = ReloadPackages({.Packages = {Base->GetPackage()}});
	ASSERT_TRUE(Reload.Wait());
	EXPECT_NE(Instance->GetParent(), Base);
	EXPECT_TRUE(Model.NeedsRefresh());
	EXPECT_TRUE(Model.Refresh());
	ASSERT_NE(FindEntry(Model, Id), nullptr);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 1.f);
	EXPECT_EQ(FindEntry(Model, Id)->Source, Instance->GetParent());
}
