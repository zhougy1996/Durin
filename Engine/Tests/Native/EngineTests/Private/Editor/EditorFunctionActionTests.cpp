#include "PropertyEditor/EditorFunctionAction.h"
#include "PropertyEditor/DetailLayoutBuilder.h"
#include "PropertyEditor/DetailPanel.h"
#include "MonaImGui.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/FunctionParameters.h"
#include "Collision/CollisionTypes.h"
#include "Components/PointLightComponent.h"
#include "Rendering/LightSceneProxy.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "EngineTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;

	class DFunctionTarget : public DObject
	{
	public:
		explicit DFunctionTarget(const FObjectInitializer& Initializer) : DObject(Initializer) { SetFlags(EObjectFlags::Transactional); }
		static void __DefaultConstructor(const FObjectInitializer& Initializer)
		{ new (Initializer.GetObj()) DFunctionTarget(Initializer); }
		virtual void Run() { ++Invocations; Value += 5; if (DuringCall) DuringCall(); }
		auto ProcessEvent(const DFunction* Function, FMutableByteView Parameters) -> EFunctionInvokeError override
		{
			++EventDispatches;
			return DObject::ProcessEvent(Function, Parameters);
		}
		void PostEditChangeProperty(const FPropertyChangedEvent&) override { DerivedValue = Value * 2; }
		static auto StaticClass() -> DClass*
		{
			static DClass* Class = [] {
				auto* Result = new DClass(EC_StaticConstructor, "Tests::DFunctionTarget",
					sizeof(DFunctionTarget), alignof(DFunctionTarget), EObjectFlags::NoFlags,
					EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass, &InternalConstructor<DFunctionTarget>);
				Result->SetSuperStructBase(DObject::StaticClass());
				Result->SetTypeNames("DFunctionTarget", "", "");
				auto* Property = new FNumericProperty(FFieldVariant(Result), "Value", EObjectFlags::NoFlags,
					EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DFunctionTarget, Value), sizeof(int32),
					DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
				Property->SetValueLifecycle(sizeof(int32), alignof(int32),
					[](void* Memory) { std::construct_at(static_cast<int32*>(Memory)); },
					[](void* Memory) { std::destroy_at(static_cast<int32*>(Memory)); });
				Result->SetChildProperties(Property);
				Result->Register(DClass::StaticClass, "", "DFunctionTarget");
				DObjectForceRegistration(Result);
				Result->RegisterFunctions({DurinCodeGen::ConstructDFunction(Result, {"Run", nullptr, EFunctionFlags::Native | EFunctionFlags::CallInEditor | EFunctionFlags::Virtual, "Actions", "Run Action", "Test action", [](DObject& Target, FFunctionFrame&, void*) { static_cast<DFunctionTarget&>(Target).Run(); }})});
				return Result;
			}();
			return Class;
		}
		int32 Value = 0;
		int32 DerivedValue = 0;
		int32 Invocations = 0;
		int32 EventDispatches = 0;
		std::function<void()> DuringCall;
	};

	class DDerivedFunctionTarget : public DFunctionTarget
	{
	public:
		explicit DDerivedFunctionTarget(const FObjectInitializer& Initializer) : DFunctionTarget(Initializer) {}
		static void __DefaultConstructor(const FObjectInitializer& Initializer)
		{ new (Initializer.GetObj()) DDerivedFunctionTarget(Initializer); }
		void Run() override { ++Invocations; Value += 10; }
		static auto StaticClass() -> DClass*
		{
			static DClass* Class = [] {
				auto* Result = new DClass(EC_StaticConstructor, "Tests::DDerivedFunctionTarget",
					sizeof(DDerivedFunctionTarget), alignof(DDerivedFunctionTarget), EObjectFlags::NoFlags,
					EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass, &InternalConstructor<DDerivedFunctionTarget>);
				Result->SetSuperStructBase(DFunctionTarget::StaticClass());
				Result->Register(DClass::StaticClass, "", "DDerivedFunctionTarget");
				DObjectForceRegistration(Result);
				return Result;
			}();
			return Class;
		}
	};

	template<typename T>
	auto MakeActionTestClass(const char* Name, DClass* Base) -> DClass*
	{
		auto* Class = new DClass(EC_StaticConstructor, FName(Name), sizeof(T), alignof(T), EObjectFlags::NoFlags,
			EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass, &InternalConstructor<T>);
		Class->SetSuperStructBase(Base);
		Class->Register(DClass::StaticClass, "", Name);
		DObjectForceRegistration(Class);
		return Class;
	}

	class DParameterizedFunctionTarget : public DFunctionTarget
	{
	public:
		explicit DParameterizedFunctionTarget(const FObjectInitializer& Init) : DFunctionTarget(Init) {}
		static void __DefaultConstructor(const FObjectInitializer& Init) { new (Init.GetObj()) DParameterizedFunctionTarget(Init); }
		static auto StaticClass() -> DClass*
		{
			static auto* Class = [] {
				auto* Result = MakeActionTestClass<DParameterizedFunctionTarget>("Tests::DParameterizedFunctionTarget", DFunctionTarget::StaticClass());
				struct FParams { int32 Value; int32 ReturnValue; };
				using namespace DurinCodeGen;
				TNativePropertyParams<int32> Value("Value", 0, EPropertyFlags::Parm);
				TNativePropertyParams<int32> Return("ReturnValue", offsetof(FParams, ReturnValue), EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
				const FPropertyParamsBase* Properties[]{&Value.Params, &Return.Params};
				auto Layout = MakeFunctionLayoutParams<FParams>(Properties, std::size(Properties));
				Result->RegisterFunctions({DurinCodeGen::ConstructDFunction(Result, {"WithArgs", &Layout, EFunctionFlags::Native | EFunctionFlags::CallInEditor, "Actions", "With Args", "", +[](DObject&, FFunctionFrame& Frame, void* ReturnValue) {
						*static_cast<int32*>(ReturnValue) = Frame.GetParameter<int32>(0) + 1;
					}})});
				return Result;
			}();
			return Class;
		}
	};

	class DOptOutFunctionTarget : public DDerivedFunctionTarget
	{
	public:
		explicit DOptOutFunctionTarget(const FObjectInitializer& Init) : DDerivedFunctionTarget(Init) {}
		static void __DefaultConstructor(const FObjectInitializer& Init) { new (Init.GetObj()) DOptOutFunctionTarget(Init); }
		static auto StaticClass() -> DClass*
		{
			static auto* Class = [] {
				auto* Result = MakeActionTestClass<DOptOutFunctionTarget>("Tests::DOptOutFunctionTarget", DDerivedFunctionTarget::StaticClass());
				Result->RegisterFunctions({DurinCodeGen::ConstructDFunction(Result, {"Run", nullptr, EFunctionFlags::Native | EFunctionFlags::Virtual, "Other", "Hidden Action", "", [](DObject& Object, FFunctionFrame&, void*) { static_cast<DFunctionTarget&>(Object).Run(); }})});
				return Result;
			}();
			return Class;
		}
	};


	auto Request(DObject* Object, FName Name = "Run") -> FEditorFunctionRequest
	{
		const auto* Function = Object->GetClass()->FindFunctionByName(Name);
		return {FObjectKey(Object), Name, Function->GetRootFunction().GetOuterDClass()->GetQualifiedName()};
	}

	auto MakeActionPackage() -> DPackage*
	{
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/FunctionActionTests/",
			(Testing::GetTestWorkDirectory() / "FunctionActionTests").generic_string() + "/");
		static uint32 Next = 0;
		const auto Name = "Package" + std::to_string(++Next);
		FPackagePath Path;
		require(FPackagePath::TryCreate("/FunctionActionTests/" + Name, Path));
		auto* Package = NewObject<DPackage>(nullptr, FName(Name));
		Package->InitializeAssetPackage(Path);
		return Package;
	}

	class FEditorFunctionTests : public testing::Test
	{
	protected:
		void SetUp() override { InitializeDObjectSystem(); }
	};
}

TEST_F(FEditorFunctionTests, InheritedVirtualActionRecordsValuesAndNeverReinvokesOnReplay)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DDerivedFunctionTarget> Target(NewObject<DDerivedFunctionTarget>(nullptr, "InheritedAction"));
	FPropertyView View;
	FPropertyViewContext Context{};
	const auto Action = ExecuteEditorFunction(View, Context, Request(Target));
	EXPECT_EQ(Action.Error, EEditorFunctionError::None);
	EXPECT_TRUE(Action.bInvoked);
	EXPECT_TRUE(Action.bChanged);
	EXPECT_EQ(Target->Value, 10);
	EXPECT_EQ(Target->DerivedValue, 20);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	EXPECT_EQ(Target->DerivedValue, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 10);
	EXPECT_EQ(Target->DerivedValue, 20);
	EXPECT_EQ(Target->Invocations, 1);
	EXPECT_EQ(Target->EventDispatches, 1);
	uint32 Count = 0;
	Target->GetClass()->ForEachFunction([&](const auto&) { ++Count; });
	EXPECT_EQ(Count, 1u);
	EXPECT_EQ(Target->GetClass()->FindFunctionByName("Run", false), nullptr);
	TStrongObjectPtr<DObject> Wrong(NewObject<DObject>(nullptr, "WrongActionType"));
	EXPECT_EQ(Wrong->ProcessEvent(Target->GetClass()->FindFunctionByName("Run"), {}), EFunctionInvokeError::WrongClass);
}

TEST_F(FEditorFunctionTests, AdmissionAndReentrancyPreventNativeMutation)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(nullptr, "RejectedAction"));
	FPropertyView View;
	FPropertyViewContext Context{.bReadOnly = true};
	EXPECT_EQ(ExecuteEditorFunction(View, Context, Request(Target)).Error, EEditorFunctionError::ReadOnly);
	Context.bReadOnly = false;
	Context.TransactionMode = ETransactionMode::NotTransactable;
	EXPECT_EQ(ExecuteEditorFunction(View, Context, Request(Target)).Error, EEditorFunctionError::MissingTransactor);
	Context.TransactionMode = ETransactionMode::Record;
	FScopedTransaction Busy(Buffer.Get(), {"test", "Busy"});
	EXPECT_TRUE(ExecuteEditorFunction(View, Context, Request(Target)).bInvoked);
	(void)Busy.Cancel();
	EXPECT_EQ(Target->Invocations, 1);
	Target->DuringCall = [&] {
		EXPECT_EQ(ExecuteEditorFunction(View, Context, Request(Target)).Error, EEditorFunctionError::Reentrant);
	};
	EXPECT_TRUE(ExecuteEditorFunction(View, Context, Request(Target)).bInvoked);
	EXPECT_EQ(Target->Invocations, 2);
	const auto OldRequest = Request(Target);
	MarkObjectHierarchyAsGarbage(Target);
	EXPECT_EQ(CanExecuteEditorFunction(Context, OldRequest), EEditorFunctionError::InvalidTarget);
}

TEST_F(FEditorFunctionTests, OversizedActionRemainsUndoableAndPreservesCheckpoint)
{
	Tests::FTestEditorTransactorOwner Buffer("OversizedAction", 1);
	auto* Package = MakeActionPackage();
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(Package, "FailedHistoryAction"));
	Buffer->EstablishSavedState(*Package);
	FPropertyView View;
	std::string Error;
	FPropertyViewContext Context{.ReportError = [&](std::string Message) { Error = std::move(Message); }};
	const auto Result = ExecuteEditorFunction(View, Context, Request(Target));
	EXPECT_TRUE(Result.bInvoked);
	EXPECT_EQ(Result.Error, EEditorFunctionError::None);
	EXPECT_EQ(Target->Value, 5);
	EXPECT_TRUE(Error.empty());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->GetPackageRevisionState(*Package));
	EXPECT_TRUE(Buffer->GetPackageRevisionState(*Package)->bCheckpointValid);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	EXPECT_FALSE(Package->IsDirty());
}

TEST_F(FEditorFunctionTests, LayoutDeduplicatesHidesAndSearchesInheritedActions)
{
	TStrongObjectPtr<DDerivedFunctionTarget> Target(NewObject<DDerivedFunctionTarget>(nullptr, "ActionLayout"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Run");
	FDetailLayoutBuilder Layout("Test action");
	Layout.ApplyRegisteredCustomizations(Target);
	Layout.ReplaceDefaultProperties();
	Layout.EditCategory("Tools").AddFunction(*Function);
	Layout.AddDefaultFunctions();
	EXPECT_EQ(Layout.GetVisibleRowCount(), 1u);
	Layout.HideFunction(*Function);
	EXPECT_EQ(Layout.GetVisibleRowCount(), 0u);
	FDetailLayoutBuilder Category;
	Category.ApplyRegisteredCustomizations(Target);
	Category.AddDefaultFunctions();
	Category.HideCategory("Actions");
	EXPECT_EQ(Category.GetVisibleRowCount(), 0u);
}

TEST_F(FEditorFunctionTests, GeneratedPrivatePointLightActionSupportsUndoAndNoOp)
{
	Tests::FTestEditorTransactorOwner Buffer;
	auto* Package = MakeActionPackage();
	TStrongObjectPtr<DPointLightComponent> Light(NewObject<DPointLightComponent>(Package, "NormalizeLight"));
	auto* Intensity = Light->GetClass()->FindPropertyByName("Intensity");
	ASSERT_NE(Intensity, nullptr);
	*Intensity->ContainerPtrToValuePtr<float>(Light.Get()) = -3.0f;
	Buffer->EstablishSavedState(*Package);
	FPropertyView View;
	FPropertyViewContext Context{};
	const auto ActionRequest = Request(Light, "NormalizeSettings");
	const auto Action = ExecuteEditorFunction(View, Context, ActionRequest);
	ASSERT_EQ(Action.Error, EEditorFunctionError::None);
	EXPECT_TRUE(Action.bChanged);
	EXPECT_EQ(*Intensity->ContainerPtrToValuePtr<float>(Light.Get()), 0.0f);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(*Intensity->ContainerPtrToValuePtr<float>(Light.Get()), -3.0f);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(*Intensity->ContainerPtrToValuePtr<float>(Light.Get()), 0.0f);
	Buffer->MarkSaved(*Package);
	const auto NoOp = ExecuteEditorFunction(View, Context, ActionRequest);
	EXPECT_TRUE(NoOp.bInvoked);
	EXPECT_FALSE(NoOp.bChanged);
	EXPECT_EQ(NoOp.Error, EEditorFunctionError::None);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	const auto* Default = DPointLightComponent::StaticClass()->GetDefaultObject();
	ASSERT_NE(Default, nullptr);
	auto TemplateRequest = ActionRequest;
	TemplateRequest.Target = FObjectKey(Default);
	EXPECT_EQ(CanExecuteEditorFunction(Context, TemplateRequest), EEditorFunctionError::TemplateTarget);
}

TEST_F(FEditorFunctionTests, ActivePropertyEditCommitsBeforeIndependentAction)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(nullptr, "EditedBeforeAction"));
	FPropertyView View;
	FPropertyViewContext Context{};
	const auto Edit = FPropertyEditTarget::ForMember(Target, Target->GetClass()->FindPropertyByName("Value"));
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Edit, [](FProperty* Property, void* Storage, uint32 Index) {
		*Property->ContainerPtrToValuePtr<int32>(Storage, Index) = 7;
	}, true));
	ASSERT_TRUE(View.IsEditing());
	const auto Result = ExecuteEditorFunction(View, Context, Request(Target));
	ASSERT_EQ(Result.Error, EEditorFunctionError::None);
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Target->Value, 12);
	ASSERT_EQ(Buffer->GetQueueLength(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 7);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
}

TEST_F(FEditorFunctionTests, DisabledAndMissingEditorScopesRemainInert)
{
	Tests::FTestEditorTransactorOwner Buffer;
	FScopedTransaction Missing(nullptr, {"test", "Missing"});
	EXPECT_FALSE(Missing.IsOutstanding());
	FScopedTransaction Disabled(Buffer.Get(), {"test", "Disabled"}, false);
	EXPECT_FALSE(Disabled.IsOutstanding());
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST_F(FEditorFunctionTests, DetailsClickExecutesOnlyAfterAllRowsAndRequestsRefresh)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(nullptr, "ClickedAction"));
	FPropertyView View;
	FPropertyViewContext Context{};
	struct FImGuiScope
	{
		ImGuiContext* Context = ImGui::CreateContext();
		~FImGuiScope() { ImGui::DestroyContext(Context); }
	} ImScope;
	ImGui::GetIO().DisplaySize = {1000.0f, 800.0f};
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::GetIO().Fonts->Build();
	ImVec2 ButtonMin{}, ButtonMax{};
	auto Frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({800.0f, 600.0f});
		ImGui::Begin("Function Action Click Test");
		const auto Result = DrawDetailPanel(View, Context, Target, {
			.ExtendLayout = [&](FDetailLayoutBuilder& Layout) {
				Layout.ReplaceDefaultProperties();
				Layout.AddFunction(*Target->GetClass()->FindFunctionByName("Run"));
				Layout.AddCustomRow("after action", [&](auto&, const auto&) {
					ButtonMin = ImGui::GetItemRectMin();
					ButtonMax = ImGui::GetItemRectMax();
					EXPECT_EQ(Target->Invocations, 0);
					return false;
				});
			}});
		ImGui::End();
		ImGui::Render();
		return Result;
	};
	EXPECT_FALSE(Frame().bActionInvoked);
	EXPECT_FALSE(Frame().bActionInvoked); // Settle the new table's column widths.
	ASSERT_GT(ButtonMax.x, ButtonMin.x);
	ImGui::GetIO().AddMousePosEvent((ButtonMin.x + ButtonMax.x) * 0.5f, (ButtonMin.y + ButtonMax.y) * 0.5f);
	EXPECT_FALSE(Frame().bActionInvoked); // Establish the hovered window before pressing.
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	EXPECT_FALSE(Frame().bActionInvoked);
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	const auto Clicked = Frame();
	EXPECT_TRUE(Clicked.bActionInvoked);
	EXPECT_TRUE(Clicked.bRefreshRequested);
	EXPECT_TRUE(Clicked.bChanged);
	EXPECT_EQ(Target->Invocations, 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
}

TEST_F(FEditorFunctionTests, ParameterizedFunctionsAreReflectedButExcludedFromEditorActions)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DParameterizedFunctionTarget> Target(NewObject<DParameterizedFunctionTarget>(nullptr, "ParameterizedEditorAction"));
	FPropertyViewContext Context{};
	const auto* Function = Target->GetClass()->FindFunctionByName("WithArgs");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(CanExecuteEditorFunction(Context, Request(Target, "WithArgs")), EEditorFunctionError::FunctionUnavailable);
	FDetailLayoutBuilder Layout;
	Layout.ApplyRegisteredCustomizations(Target);
	Layout.AddDefaultFunctions();
	EXPECT_EQ(Layout.GetVisibleRowCount(), 1u); // Only the inherited void Run() action.
	Layout.AddFunction(*Function);
	EXPECT_EQ(Layout.GetVisibleRowCount(), 1u);
	FPropertyView View;
	EXPECT_FALSE(ExecuteEditorFunction(View, Context, Request(Target, "WithArgs")).bInvoked);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	FFunctionParameterBuffer Parameters(*Function);
	Parameters.GetParameter<int32>(0) = 4;
	EXPECT_EQ(Target->ProcessEvent(Function, Parameters.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Parameters.GetReturnValue<int32>(), 5);
}

TEST_F(FEditorFunctionTests, EffectiveOverrideCanRemoveTheInheritedEditorButton)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DOptOutFunctionTarget> Target(NewObject<DOptOutFunctionTarget>(nullptr, "OptOutAction"));
	FPropertyViewContext Context{};
	EXPECT_EQ(CanExecuteEditorFunction(Context, Request(Target)), EEditorFunctionError::FunctionUnavailable);
	const auto* Effective = Target->GetClass()->FindFunctionByName("Run");
	EXPECT_EQ(Effective->GetDisplayName(), "Hidden Action");
	EXPECT_EQ(Effective->GetOuterDClass(), DOptOutFunctionTarget::StaticClass());
	EXPECT_EQ(Effective->GetRootFunction().GetOuterDClass(), DFunctionTarget::StaticClass());
	FDetailLayoutBuilder Layout;
	Layout.ApplyRegisteredCustomizations(Target);
	Layout.AddDefaultFunctions();
	EXPECT_EQ(Layout.GetVisibleRowCount(), 0u);
	EXPECT_EQ(Target->ProcessEvent(Effective, {}), EFunctionInvokeError::None);
	EXPECT_EQ(Target->Value, 10);
}

TEST_F(FEditorFunctionTests, NonTransactionalTargetStillExecutesNativeAction)
{
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(nullptr, "UnrecordedAction"));
	Target->ClearFlags(EObjectFlags::Transactional);
	FPropertyView View;
	FPropertyViewContext Context{};
	const auto Result = ExecuteEditorFunction(View, Context, Request(Target));
	EXPECT_EQ(Result.Error, EEditorFunctionError::None);
	EXPECT_TRUE(Result.bInvoked);
	EXPECT_FALSE(Result.bChanged);
	EXPECT_EQ(Target->Invocations, 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST_F(FEditorFunctionTests, OversizedPropertyEditCompletesBeforeActionAndLatestActionRemainsUndoable)
{
	Tests::FTestEditorTransactorOwner Buffer("OversizedAction", 1);
	TStrongObjectPtr<DFunctionTarget> Target(NewObject<DFunctionTarget>(nullptr, "UnfinishedActionEdit"));
	FPropertyView View;
	FPropertyViewContext Context{};
	const auto Edit = FPropertyEditTarget::ForMember(Target, Target->GetClass()->FindPropertyByName("Value"));
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Edit, [](FProperty* Property, void* Storage, uint32 Index) {
		*Property->ContainerPtrToValuePtr<int32>(Storage, Index) = 7;
	}, true));
	const auto Result = ExecuteEditorFunction(View, Context, Request(Target));
	EXPECT_EQ(Result.Error, EEditorFunctionError::None);
	EXPECT_TRUE(Result.bInvoked);
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Target->Invocations, 1);
	EXPECT_EQ(Target->Value, 12);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 7);
	EXPECT_FALSE(Buffer->CanUndo());
}

TEST_F(FEditorFunctionTests, RootIdentityDoesNotDependOnParentDescriptorRegistrationOrder)
{
	auto* Base = MakeActionTestClass<DFunctionTarget>("Tests::DLateFunctionBase", DObject::StaticClass());
	auto* Derived = MakeActionTestClass<DFunctionTarget>("Tests::DEarlyFunctionDerived", Base);
	const auto Flags = EFunctionFlags::Native | EFunctionFlags::CallInEditor | EFunctionFlags::Virtual;
	const auto Invoke = +[](DObject&, FFunctionFrame&, void*) {};
	Derived->RegisterFunctions({DurinCodeGen::ConstructDFunction(Derived, {"LateAction", nullptr, Flags, "Actions", "Late Action", "", Invoke})});
	Base->RegisterFunctions({DurinCodeGen::ConstructDFunction(Base, {"LateAction", nullptr, Flags, "Actions", "Late Action", "", Invoke})});
	const auto* Function = Derived->FindFunctionByName("LateAction");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Function->GetRootFunction().GetOuterDClass(), Base);
	uint32 Count = 0;
	Derived->ForEachFunction([&](const auto& Effective) {
		++Count;
		EXPECT_EQ(Effective.GetOuterDClass(), Derived);
		EXPECT_EQ(Effective.GetRootFunction().GetOuterDClass(), Base);
	});
	EXPECT_EQ(Count, 1u);
}

TEST_F(FEditorFunctionTests, NativeEnumMetadataResolvesAcrossTheEngineModuleBoundary)
{
	const auto* Enum = StaticEnum<ECollisionResponse>();
	ASSERT_NE(Enum, nullptr);
	EXPECT_EQ(Enum, FindEnumByQualifiedName("Durin::ECollisionResponse"));
	const auto Property = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<ECollisionResponse>("Response", 0).Params);
	EXPECT_EQ(static_cast<const FEnumProperty*>(Property.get())->GetEnum(), Enum);
	EXPECT_TRUE(Private::FunctionTypeMatches<ECollisionResponse>(*Property));
	EXPECT_FALSE(Private::FunctionTypeMatches<ECollisionEnabled>(*Property));
}
