#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/FunctionParameters.h"
#include "DObject/PropertyValueIterator.h"
#include "DObject/PropertyValidation.h"
#include "FunctionReflectionTestTypes.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	int32 GFunctionInvocations = 0;
	DClass* GRecursiveFunctionClass = nullptr;
	int32 GParameterConstructed = 0, GParameterDestroyed = 0;
	auto FunctionSoftPath(const char* Name) -> FObjectPath
	{
		static const bool Mounted = [] {
			Testing::RegisterMountPointForTests("/FunctionRefs/", (Testing::GetTestWorkDirectory() / "FunctionRefs").generic_string() + "/");
			return true;
		}();
		(void)Mounted;
		FObjectPath Path;
		require(FObjectPath::TryCreate(std::string("/FunctionRefs/") + Name + "." + Name, Path));
		return Path;
	}
	struct FObservedParameters
	{
		std::string Text;
		FObservedParameters() { ++GParameterConstructed; }
		~FObservedParameters() { ++GParameterDestroyed; }
	};
	struct FDeferredFunctionPayload
	{
		TObjectPtr<DObject> Object;
		DObject* Raw = nullptr;
		static auto StaticStruct() -> DStruct*
		{
			static auto* Struct = [] {
				auto* Type = new DStruct(EC_StaticConstructor, "Tests::FDeferredFunctionPayload", "FDeferredFunctionPayload",
					sizeof(FDeferredFunctionPayload), alignof(FDeferredFunctionPayload), EObjectFlags::Transient);
				Type->Register(DStruct::StaticClass, "", "FDeferredFunctionPayload");
				DObjectForceRegistration(Type);
				Type->InitializeOps(&GetDStructOps<FDeferredFunctionPayload>());
				return Type;
			}();
			return Struct;
		}
	};
	struct FNativeReferenceFunctionPayload
	{
		DObject* Object = nullptr; // Only the native collector declares this reference.
		static auto StaticStruct() -> DStruct*
		{
			static auto* Struct = [] {
				static const FDStructOps Ops = [] {
					auto Result = GetDStructOps<FNativeReferenceFunctionPayload>();
					Result.Flags |= EDStructOpsFlags::CollectReferences;
					Result.CollectReferences = +[](void* Value, FReferenceCollector& Collector) {
						Collector.AddReferencedObject(static_cast<FNativeReferenceFunctionPayload*>(Value)->Object);
					};
					return Result;
				}();
				auto* Type = new DStruct(EC_StaticConstructor, "Tests::FNativeReferenceFunctionPayload", "FNativeReferenceFunctionPayload",
					sizeof(FNativeReferenceFunctionPayload), alignof(FNativeReferenceFunctionPayload), EObjectFlags::Transient);
				Type->Register(DStruct::StaticClass, "", "FNativeReferenceFunctionPayload");
				DObjectForceRegistration(Type);
				Type->InitializeOps(&Ops);
				return Type;
			}();
			return Struct;
		}
	};

	template<typename T = DObject>
	auto MakeFunctionClass(const char* Name, DClass* Base = nullptr) -> DClass*
	{
		Testing::InitializeDObjectSystemForTests();
		auto* Class = new DClass(EC_StaticConstructor, Name, sizeof(T), alignof(T),
			EObjectFlags::NoFlags, EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass,
			&InternalConstructor<T>);
		Class->SetSuperStructBase(Base ? Base : DObject::StaticClass());
		Class->Register(DClass::StaticClass, "", Name);
		DObjectForceRegistration(Class);
		return Class;
	}

	auto MakeFunction(DClass* Owner, const char* Name, EFunctionFlags Flags = EFunctionFlags::Virtual,
		const DurinCodeGen::FFunctionLayoutParams* Layout = nullptr,
		FNativeFunctionPtr Native = +[](DObject&, FFunctionFrame&, void*) {}) -> DFunction*
	{
		return DurinCodeGen::ConstructDFunction(Owner, {Name, Layout, Flags | EFunctionFlags::Native, "Actions", Name, "", Native});
	}

	auto MakeFunction(DClass* Owner, const char* Name, const DurinCodeGen::FFunctionLayoutParams& Layout,
		FNativeFunctionPtr Native = +[](DObject&, FFunctionFrame&, void*) {}) -> DFunction*
	{
		return MakeFunction(Owner, Name, EFunctionFlags::None, &Layout, Native);
	}

	class DRecursiveFunctionTarget : public DObject
	{
	public:
		explicit DRecursiveFunctionTarget(const FObjectInitializer& Initializer) : DObject(Initializer) {}
		static void __DefaultConstructor(const FObjectInitializer& Initializer)
		{ new (Initializer.GetObj()) DRecursiveFunctionTarget(Initializer); }
		TObjectPtr<DObject> Self;
		int32 Value = 0;
	};

	auto ConstructRecursiveFunctionClass() -> DClass*
	{
		using namespace DurinCodeGen;
		static const auto Self = FObjectPropertyParams::ObjectPtr<DObject>("Self", EPropertyFlags::Transient,
			1, STRUCT_OFFSET_UINT16(DRecursiveFunctionTarget, Self), &ConstructRecursiveFunctionClass);
		static const FInt32PropertyParams Value{"Value", EPropertyFlags::None, 1,
			STRUCT_OFFSET_UINT16(DRecursiveFunctionTarget, Value)};
		static const FPropertyParamsBase* Properties[]{&Self, &Value};
		static const FFunctionParams Functions[]{
			{"Run", nullptr, EFunctionFlags::Native,
				"Actions", "Run", "", +[](DObject&, FFunctionFrame&, void*) {}}};
		const FClassParams Params{+[] { return GRecursiveFunctionClass; }, "Tests::DRecursiveFunctionTarget",
			"DRecursiveFunctionTarget", Properties, std::size(Properties), nullptr, nullptr, nullptr, 0,
			Functions, std::size(Functions)};
		return ConstructDClass(Params);
	}
}

TEST(DFunctionReflectionTests, RecursivePropertyResolutionInstallsFunctionsOnlyOnce)
{
	GRecursiveFunctionClass = MakeFunctionClass<DRecursiveFunctionTarget>("Tests::DRecursiveFunctionTarget");
	auto* Class = ConstructRecursiveFunctionClass();
	ASSERT_EQ(Class, GRecursiveFunctionClass);
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	ASSERT_NE(Class->FindFunctionByName("Run", false), nullptr);
	uint32 Count = 0;
	Class->ForEachProperty([&](FProperty*) { ++Count; }, false);
	EXPECT_EQ(Count, 2u);
}

TEST(DFunctionReflectionTests, LinkingResolvesLateParentsAndCachesEffectiveSlots)
{
	auto* Base = MakeFunctionClass("Tests::DFunctionLinkBase");
	auto* Middle = MakeFunctionClass("Tests::DFunctionLinkMiddle", Base);
	auto* Derived = MakeFunctionClass("Tests::DFunctionLinkDerived", Middle);
	Derived->RegisterFunctions({MakeFunction(Derived, "Run"), MakeFunction(Derived, "Local")});
	Base->RegisterFunctions({MakeFunction(Base, "Run"), MakeFunction(Base, "Inherited")});
	EXPECT_FALSE(Base->AreFunctionsLinked());
	ASSERT_EQ(Derived->LinkFunctions(), EFunctionLinkError::None);
	ASSERT_TRUE(Base->AreFunctionsLinked());
	ASSERT_TRUE(Middle->AreFunctionsLinked());
	const auto* Function = Derived->FindFunctionByName("Run");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Function->GetSuperFunction(), Base->FindFunctionByName("Run"));
	EXPECT_EQ(&Function->GetRootFunction(), Base->FindFunctionByName("Run"));
	EXPECT_TRUE(Function->IsSignatureCompatibleWith(*Function->GetSuperFunction()));
	EXPECT_TRUE((Function->GetNumParms() == 0));
	EXPECT_EQ(Derived->FindFunctionByName("Inherited", false), nullptr);
	EXPECT_EQ(Derived->FindFunctionByName("Missing"), nullptr);
	std::vector<FName> Names;
	Derived->ForEachFunction([&](const DFunction& Entry) { Names.push_back(Entry.GetFName()); });
	EXPECT_EQ(Names, (std::vector<FName>{"Inherited", "Run", "Local"}));
	Names.clear();
	Derived->ForEachFunction([&](const DFunction& Entry) { Names.push_back(Entry.GetFName()); }, false);
	EXPECT_EQ(Names, (std::vector<FName>{"Run", "Local"}));
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Derived->FindFunctionByName("Run"), Function);
}

TEST(DFunctionReflectionTests, LinkingRejectsNonVirtualHidingRegardlessOfRegistrationOrder)
{
	for (const bool bDerivedFirst : {false, true})
	{
		auto* Base = MakeFunctionClass(bDerivedFirst ? "Tests::DLateNonVirtualBase" : "Tests::DEarlyNonVirtualBase");
		auto* Derived = MakeFunctionClass(bDerivedFirst ? "Tests::DEarlyHidingDerived" : "Tests::DLateHidingDerived", Base);
		if (bDerivedFirst) Derived->RegisterFunctions({MakeFunction(Derived, "Run")});
		Base->RegisterFunctions({MakeFunction(Base, "Run", EFunctionFlags::None)});
		if (!bDerivedFirst) Derived->RegisterFunctions({MakeFunction(Derived, "Run")});
		EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::NonVirtualOverride);
		EXPECT_FALSE(Derived->AreFunctionsLinked());
		EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::NonVirtualOverride);
	}
}

TEST(DFunctionReflectionTests, LinkingRejectsFinalOverrideThroughAnIntermediateClass)
{
	auto* Base = MakeFunctionClass("Tests::DFinalFunctionBase");
	auto* Middle = MakeFunctionClass("Tests::DFinalFunctionMiddle", Base);
	auto* Derived = MakeFunctionClass("Tests::DFinalFunctionDerived", Middle);
	Derived->RegisterFunctions({MakeFunction(Derived, "Run")});
	Base->RegisterFunctions({MakeFunction(Base, "Run", EFunctionFlags::Virtual | EFunctionFlags::Final)});
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::FinalOverride);
	EXPECT_FALSE(Derived->AreFunctionsLinked());
}

TEST(DFunctionReflectionTests, InvocationRequiresLinkedMetadataAndALiveCompatibleTarget)
{
	auto* Class = MakeFunctionClass("Tests::DFunctionInvocationTarget");
	GFunctionInvocations = 0;
	auto* Function = DurinCodeGen::ConstructDFunction(Class, {"Run", nullptr, EFunctionFlags::Native, "Actions", "Run", "", +[](DObject&, FFunctionFrame&, void*) { ++GFunctionInvocations; }});
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "FunctionInvocationTarget"));
	ASSERT_TRUE(Target);
	EXPECT_EQ(Target->ProcessEvent(Function, {}), EFunctionInvokeError::UnlinkedFunction);
	EXPECT_EQ(GFunctionInvocations, 0);
	Class->RegisterFunctions({Function});
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	const auto* Linked = Class->FindFunctionByName("Run");
	ASSERT_NE(Linked, nullptr);
	std::byte UnexpectedParameters[1]{};
	EXPECT_EQ(Target->ProcessEvent(nullptr, {}), EFunctionInvokeError::InvalidFunction);
	EXPECT_EQ(Target->ProcessEvent(Linked, UnexpectedParameters), EFunctionInvokeError::InvalidParameters);
	EXPECT_EQ(GFunctionInvocations, 0);
	EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
	EXPECT_EQ(Target->ProcessEvent(Linked, {}), EFunctionInvokeError::None);
	EXPECT_EQ(GFunctionInvocations, 1);
	TStrongObjectPtr<DObject> Wrong(NewObject<DObject>(nullptr, "WrongFunctionTarget"));
	EXPECT_EQ(Wrong->ProcessEvent(Linked, {}), EFunctionInvokeError::WrongClass);
	// The handle stops resolving after marking. The allocated object remains valid
	// member-call storage until collection, allowing admission to reject its garbage state.
	auto* GarbageTarget = Target.Get();
	MarkAsGarbage(GarbageTarget);
	EXPECT_EQ(GarbageTarget->ProcessEvent(Linked, {}), EFunctionInvokeError::InvalidTarget);
	EXPECT_EQ(GFunctionInvocations, 1);
}

TEST(DFunctionReflectionTests, DispatchRejectsUntrackedAndUnregisteredFunctionMetadata)
{
	auto* Class = MakeFunctionClass("Tests::DFunctionMetadataAdmission");
	auto* Function = MakeFunction(Class, "Run");
	Class->RegisterFunctions({Function});
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "MetadataAdmission"));
	auto* Unregistered = MakeFunction(Class, "Unregistered");
	EXPECT_EQ(Target->ProcessEvent(Unregistered, {}), EFunctionInvokeError::UnlinkedFunction);
	DFunction Untracked(FObjectInitializer{});
	EXPECT_EQ(Target->ProcessEvent(&Untracked, {}), EFunctionInvokeError::InvalidFunction);
}

TEST(DFunctionReflectionTests, NestedProcessEventBuildsAndRestoresNativeFrames)
{
	auto* Class = MakeFunctionClass("Tests::DNestedFunctionTarget");
	GFunctionInvocations = 0;
	Class->RegisterFunctions({
		DurinCodeGen::ConstructDFunction(Class, {"Outer", nullptr, EFunctionFlags::Native, "Actions", "Outer", "", +[](DObject& Target, FFunctionFrame& Frame, void* Result) {
				EXPECT_EQ(&Frame.GetObject(), &Target);
				EXPECT_EQ(Frame.GetFunction().GetFName(), FName("Outer"));
				EXPECT_EQ(Frame.GetPreviousFrame(), nullptr);
				EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
				EXPECT_EQ(Result, nullptr);
				EXPECT_TRUE(Frame.GetParameters().empty());
				const auto* Inner = Target.GetClass()->FindFunctionByName("Inner");
				std::byte UnexpectedParameters[1]{};
				EXPECT_EQ(Target.ProcessEvent(Inner, UnexpectedParameters), EFunctionInvokeError::InvalidParameters);
				EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
				EXPECT_EQ(Target.ProcessEvent(Inner, {}), EFunctionInvokeError::None);
				EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
			}}),
		DurinCodeGen::ConstructDFunction(Class, {"Inner", nullptr, EFunctionFlags::Native, "Actions", "Inner", "", +[](DObject& Target, FFunctionFrame& Frame, void* Result) {
				ASSERT_NE(Frame.GetPreviousFrame(), nullptr);
				EXPECT_EQ(&Frame.GetObject(), &Target);
				EXPECT_EQ(Frame.GetFunction().GetFName(), FName("Inner"));
				EXPECT_EQ(Frame.GetPreviousFrame()->GetFunction().GetFName(), FName("Outer"));
				EXPECT_EQ(&Frame.GetPreviousFrame()->GetObject(), &Target);
				EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
				EXPECT_EQ(Result, nullptr);
				++GFunctionInvocations;
			}})});
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "NestedFunctionTarget"));
	ASSERT_TRUE(Target);
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Target->ProcessEvent(Class->FindFunctionByName("Outer"), {}), EFunctionInvokeError::None);
	EXPECT_EQ(GFunctionInvocations, 1);
	EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
}

TEST(DFunctionReflectionTests, NativeExceptionUnwindsTheCallFrameWithoutConvertingDomainFailure)
{
	auto* Class = MakeFunctionClass("Tests::DThrowingFunctionTarget");
	Class->RegisterFunctions({DurinCodeGen::ConstructDFunction(Class, {"Throw", nullptr, EFunctionFlags::Native, "Actions", "Throw", "", +[](DObject&, FFunctionFrame& Frame, void*) {
			EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
			throw std::runtime_error("native function body");
		}})});
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "ThrowingFunctionTarget"));
	ASSERT_TRUE(Target);
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_THROW(Target->ProcessEvent(Class->FindFunctionByName("Throw"), {}), std::runtime_error);
	EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
}

TEST(DFunctionReflectionTests, NativeDefaultsDoNotChangeBufferInitializationOrDispatch)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "NativeDefaults"));
	EXPECT_EQ(Target->NativeDefault(), 17);
	const auto* Function = Target->GetClass()->FindFunctionByName("NativeDefault");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	EXPECT_EQ(Buffer.GetParameter<int32>(0), 0);
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 0);
	Buffer.GetParameter<int32>(0) = 23;
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 23);
}

TEST(DFunctionReflectionTests, GeneratedMethodSelectionIgnoresOrdinaryOverloads)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "Overloads"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Overloaded");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	Buffer.GetParameter<std::string>(0) = "chosen";
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 6);
}

TEST(DFunctionReflectionTests, ReflectedLiteralDefaultsAreTypedOwnedAndExplicit)
{
	using namespace FunctionFixtures;
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "LiteralDefaults"));
	const auto* Function = Target->GetClass()->FindFunctionByName("LiteralDefaults");
	ASSERT_NE(Function, nullptr);
	EXPECT_TRUE(Function->GetParameterDefaultValue<bool>(0));
	EXPECT_EQ(Function->GetParameterDefaultValue<int8>(1), -128);
	EXPECT_EQ(Function->GetParameterDefaultValue<int16>(2), -32768);
	EXPECT_EQ(Function->GetParameterDefaultValue<int32>(3), std::numeric_limits<int32>::min());
	EXPECT_EQ(Function->GetParameterDefaultValue<int64>(4), -9223372036854775807LL);
	EXPECT_EQ(Function->GetParameterDefaultValue<uint8>(5), 255);
	EXPECT_EQ(Function->GetParameterDefaultValue<uint16>(6), 65535);
	EXPECT_EQ(Function->GetParameterDefaultValue<uint32>(7), 4294967295U);
	EXPECT_EQ(Function->GetParameterDefaultValue<uint64>(8), std::numeric_limits<uint64>::max());
	EXPECT_EQ(Function->GetParameterDefaultValue<float>(9), 1.25f);
	EXPECT_EQ(Function->GetParameterDefaultValue<double>(10), -2.5);
	EXPECT_EQ(Function->GetParameterDefaultValue<std::string>(11), "default\ntext");
	EXPECT_EQ(Function->GetParameterDefaultValue<EFunctionState>(12), EFunctionState::Ready);
	EXPECT_EQ(Function->GetParameterDefaultValue<DObject*>(13), nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	EXPECT_FALSE(Buffer.GetParameter<bool>(0));
	EXPECT_TRUE(Buffer.GetParameter<std::string>(11).empty());
	ASSERT_TRUE(Buffer.ApplyDefaults({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
	EXPECT_EQ(Buffer.GetParameter<uint64>(8), std::numeric_limits<uint64>::max());
	EXPECT_EQ(Buffer.GetParameter<EFunctionState>(12), EFunctionState::Ready);
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 12);
	Buffer.GetParameter<std::string>(11) = "changed";
	Buffer.GetParameter<bool>(0) = false;
	Buffer.GetParameter<DObject*>(13) = Target.Get();
	ASSERT_TRUE(Buffer.ApplyDefaults({9}));
	EXPECT_FALSE(Buffer.GetParameter<bool>(0));
	EXPECT_EQ(Buffer.GetParameter<DObject*>(13), Target.Get());
	EXPECT_EQ(Function->GetParameterDefaultValue<std::string>(11), "default\ntext");
	FFunctionParameterBuffer Another(*Function);
	ASSERT_TRUE(Another.ApplyDefaults({11}));
	EXPECT_EQ(Another.GetParameter<std::string>(11), "default\ntext");
}

TEST(DFunctionReflectionTests, DefaultRequestsPreflightAllIndexesWithoutPartialWrites)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "DefaultPreflight"));
	const auto* Function = Target->GetClass()->FindFunctionByName("PartialDefault");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Function->GetParameterDefault(0), nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	Buffer.GetParameter<int32>(1) = 99;
	auto Error = Buffer.ApplyDefaults({1, 0});
	ASSERT_FALSE(Error); EXPECT_EQ(Error.error().Code, EFunctionDefaultError::MissingDefault);
	EXPECT_EQ(Error.error().ParameterIndex, 0u);
	EXPECT_EQ(Buffer.GetParameter<int32>(1), 99);
	Error = Buffer.ApplyDefaults({1, 1});
	ASSERT_FALSE(Error); EXPECT_EQ(Error.error().Code, EFunctionDefaultError::DuplicateIndex);
	EXPECT_EQ(Buffer.GetParameter<int32>(1), 99);
	Error = Buffer.ApplyDefaults({1, 2}); // Return storage is not a parameter input.
	ASSERT_FALSE(Error); EXPECT_EQ(Error.error().Code, EFunctionDefaultError::InvalidIndex);
	EXPECT_EQ(Buffer.GetParameter<int32>(1), 99);
	ASSERT_TRUE(Buffer.ApplyDefaults({1}));
	EXPECT_EQ(Buffer.GetParameter<int32>(0), 0);
	EXPECT_EQ(Buffer.GetParameter<int32>(1), 23);
	const auto* NativeOnly = Target->GetClass()->FindFunctionByName("NativeOnlyDefault");
	ASSERT_NE(NativeOnly, nullptr);
	EXPECT_EQ(NativeOnly->GetParameterDefault(0)->State, EFunctionDefaultState::NativeOnly);
	FFunctionParameterBuffer NativeBuffer(*NativeOnly);
	Error = NativeBuffer.ApplyDefaults({0});
	ASSERT_FALSE(Error); EXPECT_EQ(Error.error().Code, EFunctionDefaultError::NativeOnly);
	EXPECT_EQ(NativeBuffer.GetParameter<int32>(0), 0);
	const auto* UnsignedNegation = Target->GetClass()->FindFunctionByName("NativeUnsignedNegation");
	ASSERT_NE(UnsignedNegation, nullptr);
	EXPECT_EQ(UnsignedNegation->GetParameterDefault(0)->State, EFunctionDefaultState::NativeOnly);
	EXPECT_EQ(Target->NativeUnsignedNegation(), 1);
	const auto* Mutable = Target->GetClass()->FindFunctionByName("MutableDefault");
	ASSERT_NE(Mutable, nullptr);
	FFunctionParameterBuffer MutableBuffer(*Mutable);
	Error = MutableBuffer.ApplyDefaults({0});
	ASSERT_FALSE(Error); EXPECT_EQ(Error.error().Code, EFunctionDefaultError::NotInput);
	const auto* Zero = Target->GetClass()->FindFunctionByName("ZeroDefault");
	ASSERT_NE(Zero, nullptr);
	EXPECT_EQ(Zero->GetParameterDefaultValue<int32>(0), 0);
	FFunctionParameterBuffer ZeroBuffer(*Zero);
	ZeroBuffer.GetParameter<int32>(0) = 7;
	ASSERT_TRUE(ZeroBuffer.ApplyDefaults({0}));
	EXPECT_EQ(ZeroBuffer.GetParameter<int32>(0), 0);
	const auto* NullReference = Target->GetClass()->FindFunctionByName("NullReferenceDefault");
	ASSERT_NE(NullReference, nullptr);
	FFunctionParameterBuffer NullBuffer(*NullReference);
	ASSERT_TRUE(NullBuffer.ApplyDefaults({0}));
	EXPECT_EQ(Target->ProcessEvent(NullReference, NullBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(NullBuffer.GetReturnValue<DObject*>(), nullptr);
}

TEST(DFunctionReflectionTests, DefaultsFollowEffectiveDescriptorsRatherThanNativeOverrideDeclarations)
{
	using namespace FunctionFixtures;
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DParameterDerived> Annotated(NewObject<DParameterDerived>(nullptr, "AnnotatedDefaults"));
	const auto* Function = Annotated->GetClass()->FindFunctionByName("NativeDefault");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Function->GetParameterDefaultValue<int32>(0), 29);
	FFunctionParameterBuffer Buffer(*Function);
	ASSERT_TRUE(Buffer.ApplyDefaults({0}));
	EXPECT_EQ(Annotated->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 58);
	TStrongObjectPtr<DDefaultInherited> Unannotated(NewObject<DDefaultInherited>(nullptr, "InheritedDefaults"));
	Function = Unannotated->GetClass()->FindFunctionByName("NativeDefault");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Function, DParameterTarget::StaticClass()->FindFunctionByName("NativeDefault"));
	EXPECT_EQ(Function->GetParameterDefaultValue<int32>(0), 17);
	EXPECT_EQ(Unannotated->NativeDefault(), 129);
	FFunctionParameterBuffer Inherited(*Function);
	ASSERT_TRUE(Inherited.ApplyDefaults({0}));
	EXPECT_EQ(Unannotated->ProcessEvent(Function, Inherited.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Inherited.GetReturnValue<int32>(), 51);
	const auto* Omitted = DDefaultOmitted::StaticClass()->FindFunctionByName("NativeDefault");
	ASSERT_NE(Omitted, nullptr);
	EXPECT_EQ(Omitted->GetParameterDefault(0), nullptr);
}

TEST(DFunctionReflectionTests, LinkingRejectsInvalidDefaultRecordsAndOwnsValidValues)
{
	using namespace DurinCodeGen;
	struct FArguments { int32 Value; };
	TNativePropertyParams<int32> Value("Value", 0, EPropertyFlags::Parm);
	const FPropertyParamsBase* Properties[]{&Value.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	int32 Counter = 0;
	auto Construct = [&](const std::vector<FFunctionParameterDefault>& Defaults, bool Valid) {
		auto* Owner = MakeFunctionClass(std::format("Tests::DDefaultRecords{}", Counter++).c_str());
		FFunctionParams Params{"Run", &Layout, EFunctionFlags::Native, "", "", "", +[](DObject&, FFunctionFrame&, void*) {}, Defaults.data(), Defaults.size()};
		auto* Function = ConstructDFunction(Owner, Params);
		Owner->RegisterFunctions({Function});
		EXPECT_EQ(Owner->LinkFunctions(), Valid ? EFunctionLinkError::None : EFunctionLinkError::InvalidDescriptor);
		return Function;
	};
	auto Default = MakeFunctionParameterDefault<int32>(0, "42", 42);
	auto* Valid = Construct({Default}, true);
	Default.Value.reset(); // Descriptor owns its copied record and value.
	EXPECT_EQ(Valid->GetParameterDefaultValue<int32>(0), 42);
	Construct({Default}, false);
	Default = MakeFunctionParameterDefault<int32>(1, "42", 42); Construct({Default}, false);
	Default.ParameterIndex = 0; Construct({Default, Default}, false);
	Construct({MakeFunctionParameterDefault<float>(0, "1.f", 1.f)}, false);
	Default.State = EFunctionDefaultState::NativeOnly; Construct({Default}, false);
	Default.State = EFunctionDefaultState::None; Construct({Default}, false);
	Default.State = EFunctionDefaultState::Reflected; Default.Expression.clear(); Construct({Default}, false);
}

TEST(DFunctionReflectionTests, DefaultLinkValidationChecksRolesEnumsAndNullObjects)
{
	using namespace DurinCodeGen;
	using namespace FunctionFixtures;
	int32 Counter = 0;
	auto Check = [&]<typename T>(T Value, FFunctionParameterDefault Default, EPropertyFlags Flags = EPropertyFlags::Parm) {
		struct FArguments { T Value; };
		TNativePropertyParams<T> Property("Value", 0, Flags);
		const FPropertyParamsBase* Properties[]{&Property.Params};
		auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
		auto* Owner = MakeFunctionClass(std::format("Tests::DDefaultTypeValidation{}", Counter++).c_str());
		FFunctionParams Params{"Run", &Layout, EFunctionFlags::Native, "", "", "", +[](DObject&, FFunctionFrame&, void*) {}, &Default, 1};
		auto* Function = ConstructDFunction(Owner, Params);
		Owner->RegisterFunctions({Function});
		EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
		(void)Value;
	};
	Check(int32{}, MakeFunctionParameterDefault<int32>(0, "1", 1), EPropertyFlags::Parm | EPropertyFlags::ReferenceParm | EPropertyFlags::OutParm);
	Check(int32{}, MakeFunctionParameterDefault<int32>(0, "1", 1), EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
	Check(EFunctionState{}, MakeFunctionParameterDefault<EOtherFunctionState>(0, "Other", EOtherFunctionState::Other));
	Check(static_cast<DObject*>(nullptr), MakeFunctionParameterDefault<DObject*>(0, "non-null", DObject::StaticClass()));
	Check(TObjectPtr<DObject>{}, MakeFunctionParameterDefault<TObjectPtr<DObject>>(0, "{}", {}));
	Check(std::string{}, FFunctionParameterDefault{0, EFunctionDefaultState::Reflected, "text"});
}

TEST(DFunctionReflectionTests, GeneratedStringArgumentsReferenceOutputAndReturnUseInitializedStorage)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "StringParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Combine");
	ASSERT_NE(Function, nullptr);
	ASSERT_EQ(Function->GetNumInputParms(), 3u);
	EXPECT_TRUE(Function->HasAnyFunctionFlags(EFunctionFlags::Const));
	EXPECT_TRUE(Function->GetParameterProperty(1)->HasAnyPropertyFlags(EPropertyFlags::ReferenceParm | EPropertyFlags::ConstParm));
	EXPECT_TRUE(Function->GetParameterProperty(2)->HasAnyPropertyFlags(EPropertyFlags::OutParm));
	ASSERT_NE(Function->GetReturnProperty(), nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	EXPECT_TRUE(Buffer.GetParameter<std::string>(1).empty());
	EXPECT_TRUE(Buffer.GetReturnValue<std::string>().empty());
	Buffer.GetParameter<int32>(0) = 42;
	Buffer.GetParameter<std::string>(1) = "value=";
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetParameter<std::string>(1), "value=");
	EXPECT_EQ(Buffer.GetParameter<std::string>(2), "value=42");
	EXPECT_EQ(Buffer.GetReturnValue<std::string>(), "value=42!");
	Buffer.GetParameter<int32>(0) = 7;
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<std::string>(), "value=7!");
	EXPECT_EQ(Target->ProcessEvent(Function, {}), EFunctionInvokeError::InvalidParameters);
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView().first(Function->GetParmsSize() - 1)), EFunctionInvokeError::InvalidParameters);
	std::vector<std::byte> Misaligned(Function->GetParmsSize() + Function->GetMinAlignment());
	size_t Offset = 0;
	while (reinterpret_cast<uintptr_t>(Misaligned.data() + Offset) % Function->GetMinAlignment() == 0) ++Offset;
	EXPECT_EQ(Target->ProcessEvent(Function, {Misaligned.data() + Offset, Function->GetParmsSize()}), EFunctionInvokeError::InvalidParameters);
}

TEST(DFunctionReflectionTests, GeneratedThunkAcceptsBorrowedCompilerLaidOutParameterAggregate)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "BorrowedParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Combine");
	ASSERT_NE(Function, nullptr);
	struct FArguments { int32 Value; std::string Prefix, Output, ReturnValue; } Arguments{6, "raw=", {}, {}};
	ASSERT_EQ(sizeof(Arguments), Function->GetParmsSize());
	ASSERT_EQ(offsetof(FArguments, ReturnValue), Function->GetReturnProperty()->GetOffset());
	EXPECT_EQ(Target->ProcessEvent(Function, std::as_writable_bytes(std::span(&Arguments, 1))), EFunctionInvokeError::None);
	EXPECT_EQ(Arguments.Output, "raw=6");
	EXPECT_EQ(Arguments.ReturnValue, "raw=6!");
}

TEST(DFunctionReflectionTests, GeneratedReturnOnlyReferenceOnlyAndMixedAlignmentSignaturesDispatch)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "MixedParameters"));
	const auto* Read = Target->GetClass()->FindFunctionByName("ReadValue");
	ASSERT_NE(Read, nullptr);
	EXPECT_FALSE((Read->GetNumParms() == 0));
	FFunctionParameterBuffer ReadBuffer(*Read);
	EXPECT_EQ(Target->ProcessEvent(Read, ReadBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(ReadBuffer.GetReturnValue<int32>(), 17);
	const auto* Update = Target->GetClass()->FindFunctionByName("Update");
	ASSERT_NE(Update, nullptr);
	EXPECT_EQ(Update->GetReturnProperty(), nullptr);
	FFunctionParameterBuffer UpdateBuffer(*Update);
	UpdateBuffer.GetParameter<int32>(0) = 3;
	EXPECT_EQ(Target->ProcessEvent(Update, UpdateBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(UpdateBuffer.GetParameter<int32>(0), 8);
	const auto* Mixed = Target->GetClass()->FindFunctionByName("Mixed");
	ASSERT_NE(Mixed, nullptr);
	FFunctionParameterBuffer MixedBuffer(*Mixed);
	MixedBuffer.GetParameter<float>(0) = 1.5f;
	MixedBuffer.GetParameter<double>(1) = 2.25;
	MixedBuffer.GetParameter<bool>(2) = true;
	EXPECT_EQ(Target->ProcessEvent(Mixed, MixedBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_DOUBLE_EQ(MixedBuffer.GetReturnValue<double>(), -3.75);
}

TEST(DFunctionReflectionTests, GeneratedNameAndGuidValuesRoundTrip)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "NameGuidParameters"));
	const auto* GuidFunction = Target->GetClass()->FindFunctionByName("EchoGuid");
	ASSERT_NE(GuidFunction, nullptr);
	FFunctionParameterBuffer GuidBuffer(*GuidFunction);
	const FGuid Guid{1, 2, 3, 4};
	GuidBuffer.GetParameter<FGuid>(0) = Guid;
	EXPECT_EQ(Target->ProcessEvent(GuidFunction, GuidBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(GuidBuffer.GetReturnValue<FGuid>(), Guid);
	const auto* NameFunction = Target->GetClass()->FindFunctionByName("EchoName");
	ASSERT_NE(NameFunction, nullptr);
	FFunctionParameterBuffer NameBuffer(*NameFunction);
	NameBuffer.GetParameter<FName>(0) = "FunctionValue";
	EXPECT_EQ(Target->ProcessEvent(NameFunction, NameBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(NameBuffer.GetReturnValue<FName>(), FName("FunctionValue"));
}

TEST(DFunctionReflectionTests, ParameterizedVirtualOverridePreservesSlotAndIgnoresParameterNames)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterDerived> Target(NewObject<FunctionFixtures::DParameterDerived>(nullptr, "OverrideParameters"));
	const auto* Base = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Scale");
	const auto* Derived = Target->GetClass()->FindFunctionByName("Scale");
	ASSERT_NE(Base, nullptr);
	ASSERT_NE(Derived, nullptr);
	EXPECT_EQ(Derived->GetSuperFunction(), Base);
	EXPECT_EQ(Derived->GetSuperStructBase(), Base);
	EXPECT_EQ(Derived->GetInheritanceSuper(), nullptr);
	EXPECT_TRUE(Derived->IsSignatureCompatibleWith(*Base));
	size_t Fields = 0;
	Derived->ForEachProperty([&](FProperty* Property) { ++Fields; EXPECT_EQ(Property->GetOwnerVariant().ToDObject(), Derived); });
	EXPECT_EQ(Fields, Derived->GetNumParms());
	FFunctionParameterBuffer Buffer(*Base);
	size_t Values = 0;
	for (FPropertyValueIterator It(Derived, Buffer.GetView().data()); It; ++It) ++Values;
	EXPECT_EQ(Values, Derived->GetNumParms());
	Buffer.GetParameter<int32>(0) = 7;
	EXPECT_EQ(Target->ProcessEvent(Derived, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 21);
	EXPECT_EQ(Target->ProcessEvent(Base, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 21);
}

TEST(DFunctionReflectionTests, LinkingFinalizesLayoutAndFailedValidationCanBeRepaired)
{
	using namespace DurinCodeGen;
	auto* Owner = MakeFunctionClass("Tests::DFunctionLinkLayout");
	struct FArguments { int32 Value; };
	TNativePropertyParams<int32> Value("Value", 0, EPropertyFlags::Parm);
	const FPropertyParamsBase* Properties[]{&Value.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	auto* Function = MakeFunction(Owner, "Run", Layout);
	EXPECT_FALSE(Function->ArePropertiesLinked());
	Function->SetStructureLayout(1, 1);
	Owner->RegisterFunctions({Function});
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
	EXPECT_EQ(Function->GetLinkDiagnostic().OwnerClass, "Tests::DFunctionLinkLayout");
	EXPECT_EQ(Function->GetLinkDiagnostic().FunctionName, "Run");
	EXPECT_EQ(Function->GetLinkDiagnostic().PropertyPath, "Value");
	EXPECT_NE(ToString(Owner->GetFunctionLinkDiagnostic()).find("Value: parameter storage exceeds the aggregate bounds"), std::string::npos);
	EXPECT_FALSE(Function->ArePropertiesLinked());
	EXPECT_FALSE(Owner->AreFunctionsLinked());
	Function->SetStructureLayout(sizeof(FArguments), alignof(FArguments));
	ASSERT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_FALSE(Function->GetLinkDiagnostic().HasError());
	EXPECT_FALSE(Owner->GetFunctionLinkDiagnostic().HasError());
	EXPECT_TRUE(ToString(Function->GetLinkDiagnostic()).empty());
	EXPECT_TRUE(Function->ArePropertiesLinked());
	EXPECT_EQ(Function->GetNumInputParms(), 1u);
	EXPECT_EQ(Function->GetParameterProperty(0), Function->GetChildProperties());
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Function->GetNumInputParms(), 1u);
}

TEST(DFunctionReflectionTests, VirtualLayoutLinkDoesNotGrantClassDispatchAdmission)
{
	auto* Owner = MakeFunctionClass("Tests::DExplicitFunctionLink");
	auto* Function = MakeFunction(Owner, "Run");
	DStructBase* Metadata = Function;
	ASSERT_TRUE(Metadata->Link());
	EXPECT_TRUE(Function->ArePropertiesLinked());
	EXPECT_FALSE(Owner->AreFunctionsLinked());
	EXPECT_EQ(Function->GetNumParms(), 0u);
	TStrongObjectPtr<DObject> Target(NewObject(Owner, nullptr, "ExplicitFunctionLink"));
	EXPECT_EQ(Target->ProcessEvent(Function, {}), EFunctionInvokeError::UnlinkedFunction);
	Owner->RegisterFunctions({Function});
	ASSERT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Target->ProcessEvent(Function, {}), EFunctionInvokeError::None);
}

TEST(DFunctionReflectionTests, ClassLinkRejectsAParentChangedAfterFunctionLayoutLink)
{
	auto* FirstBase = MakeFunctionClass("Tests::DFirstLinkedFunctionBase");
	auto* SecondBase = MakeFunctionClass("Tests::DSecondLinkedFunctionBase");
	FirstBase->RegisterFunctions({MakeFunction(FirstBase, "Run")});
	SecondBase->RegisterFunctions({MakeFunction(SecondBase, "Run")});
	auto* Owner = MakeFunctionClass("Tests::DFunctionParentChange", FirstBase);
	auto* Function = MakeFunction(Owner, "Run");
	ASSERT_TRUE(Function->Link());
	EXPECT_EQ(Function->GetSuperFunction(), FirstBase->FindFunctionByName("Run"));
	Owner->SetSuperStructBase(SecondBase);
	Owner->RegisterFunctions({Function});
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
	EXPECT_FALSE(Owner->AreFunctionsLinked());
}

TEST(DFunctionReflectionTests, FailedOverrideBatchDoesNotAdmitEarlierLocalFunctions)
{
	auto* Base = MakeFunctionClass("Tests::DFunctionAtomicBase");
	auto* Derived = MakeFunctionClass("Tests::DFunctionAtomicDerived", Base);
	Base->RegisterFunctions({MakeFunction(Base, "Run", EFunctionFlags::None)});
	auto* First = MakeFunction(Derived, "Local");
	auto* Invalid = MakeFunction(Derived, "Run");
	Derived->RegisterFunctions({First, Invalid});
	ASSERT_EQ(Derived->LinkFunctions(), EFunctionLinkError::NonVirtualOverride);
	TStrongObjectPtr<DObject> Target(NewObject(Derived, nullptr, "AtomicFunctionTarget"));
	EXPECT_EQ(Target->ProcessEvent(First, {}), EFunctionInvokeError::UnlinkedFunction);
	EXPECT_EQ(Target->ProcessEvent(Invalid, {}), EFunctionInvokeError::UnlinkedFunction);
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::NonVirtualOverride);
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, LinkedMetadataRejectsLayoutChainFlagsAndValueOperationMutation)
{
	Testing::InitializeDObjectSystemForTests();
	auto* Function = const_cast<DFunction*>(FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Combine"));
	auto* Property = const_cast<FProperty*>(Function->GetParameterProperty(0));
	EXPECT_DEATH(Function->SetStructureLayout(1, 1), "linked and cannot be modified");
	EXPECT_DEATH(Function->SetChildProperties(nullptr), "linked and cannot be modified");
	EXPECT_DEATH(Function->SetSuperStructBase(nullptr), "linked and cannot be modified");
	EXPECT_DEATH(Function->AddCppProperty(Property), "linked and cannot be modified");
	EXPECT_DEATH(Property->SetPropertyFlags(EPropertyFlags::OutParm), "linked and cannot be modified");
	EXPECT_DEATH(Property->SetNext(nullptr), "linked and cannot be modified");
	EXPECT_DEATH(Property->SetOwner(FFieldVariant(nullptr)), "linked and cannot be modified");
	EXPECT_DEATH(Property->SetValueLifecycle(0, 0, nullptr, nullptr), "linked and cannot be modified");
	EXPECT_DEATH(Property->SetValueAccessors(nullptr, nullptr), "linked and cannot be modified");
	const auto* Container = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("EchoObjects");
	auto* Array = const_cast<FArrayProperty*>(static_cast<const FArrayProperty*>(Container->GetParameterProperty(0)));
	EXPECT_DEATH(Array->GetInner()->SetPropertyFlags(EPropertyFlags::OutParm), "linked and cannot be modified");
	EXPECT_DEATH(Array->SetInner(nullptr), "linked and cannot be modified");
}
#endif

TEST(DFunctionReflectionTests, SharedPropertyConstructionPreservesOrderAndOwnership)
{
	using namespace DurinCodeGen;
	auto* Owner = MakeFunctionClass("Tests::DSharedPropertyConstruction");
	TNativePropertyParams<int32> First("First", 0);
	TNativePropertyParams<std::vector<int32>> Second("Second", 8);
	const FPropertyParamsBase* Properties[]{&First.Params, &Second.Params};
	ConstructProperties(*Owner, Properties, std::size(Properties));
	std::vector<FName> Names;
	Owner->ForEachProperty([&](FProperty* Property) {
		Names.push_back(Property->NamePrivate);
		EXPECT_EQ(Property->GetOwnerVariant().ToDObject(), Owner);
	}, false);
	EXPECT_EQ(Names, (std::vector<FName>{"First", "Second"}));
	auto* Array = static_cast<FArrayProperty*>(Owner->GetChildProperties()->GetNext());
	EXPECT_EQ(Array->GetInner()->GetOwnerVariant().ToField(), Array);
	EXPECT_EQ(Array->GetNext(), nullptr);
}

TEST(DFunctionReflectionTests, LayoutValidationRejectsOverlapMissingLifecycleAndInvalidBounds)
{
	struct FArguments { int32 Value, Result; };
	using namespace DurinCodeGen;
	TNativePropertyParams<int32> Value("Value", 0, EPropertyFlags::Parm);
	TNativePropertyParams<int32> Result("ReturnValue", offsetof(FArguments, Result), EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
	TNativePropertyParams<int32> Overlap("ReturnValue", 0, EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
	const FPropertyParamsBase* Properties[]{&Value.Params, &Result.Params};
	const FPropertyParamsBase* Overlapping[]{&Value.Params, &Overlap.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, std::size(Properties));
	auto Construct = [&](const FFunctionLayoutParams& Input, EFunctionLinkError Expected) {
		auto* Owner = MakeFunctionClass("Tests::DLayoutValidation");
		auto* Function = MakeFunction(Owner, "Run", Input);
		Owner->RegisterFunctions({Function});
		EXPECT_EQ(Owner->LinkFunctions(), Expected);
		return Function;
	};
	const auto* Valid = Construct(Layout, EFunctionLinkError::None);
	EXPECT_TRUE(Valid->IsSignatureCompatibleWith(*Valid));
	auto CheckInvalid = [&](const FFunctionLayoutParams& Input, const char* Path, const char* Reason) {
		const auto* Invalid = Construct(Input, EFunctionLinkError::InvalidDescriptor);
		EXPECT_EQ(Invalid->GetLinkDiagnostic().PropertyPath, Path);
		EXPECT_NE(Invalid->GetLinkDiagnostic().Reason.find(Reason), std::string::npos);
		EXPECT_FALSE(Invalid->IsSignatureCompatibleWith(*Valid));
		EXPECT_FALSE(Valid->IsSignatureCompatibleWith(*Invalid));
		EXPECT_FALSE(Invalid->IsSignatureCompatibleWith(*Invalid));
	};
	auto Invalid = Layout;
	Invalid.PropertyParams = Overlapping;
	CheckInvalid(Invalid, "ReturnValue", "overlaps 'Value'");
	Invalid = Layout; Invalid.Initialize = nullptr; CheckInvalid(Invalid, "", "callback is missing");
	Invalid = Layout; Invalid.ParmsSize = 1; CheckInvalid(Invalid, "", "aggregate alignment");
	Invalid = Layout; Invalid.ParmsAlignment = 3; CheckInvalid(Invalid, "", "aggregate alignment");
}

TEST(DFunctionReflectionTests, NestedStorageDiagnosticsUseTheSharedValidatorAndClearAfterRepair)
{
	using namespace DurinCodeGen;
	using FValues = std::unordered_map<std::string, std::vector<int32>>;
	struct FArguments { FValues Values; };
	auto* Owner = MakeFunctionClass("Tests::DNestedStorageDiagnostic");
	TNativePropertyParams<FValues> Values("Values", 0, EPropertyFlags::Parm);
	const FPropertyParamsBase* Properties[]{&Values.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	auto* Function = MakeFunction(Owner, "Run", Layout);
	Owner->RegisterFunctions({Function});
	auto* Map = static_cast<FMapProperty*>(Function->GetChildProperties());
	auto* Inner = static_cast<FArrayProperty*>(Map->GetValueProp())->GetInner();
	Inner->SetValueLifecycle(sizeof(int16), alignof(int16), nullptr, nullptr);
	const auto Error = ValidateNativeProperty(*Inner);
	ASSERT_TRUE(Error);
	EXPECT_EQ(Error->Code, EPropertyValidationError::InvalidStorage);
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
	const auto& Diagnostic = Owner->GetFunctionLinkDiagnostic();
	EXPECT_EQ(Diagnostic.PropertyPath, "Values.Value.Inner");
	EXPECT_EQ(Diagnostic.Reason, Error->Reason);
	EXPECT_EQ(ToString(Diagnostic), "Tests::DNestedStorageDiagnostic::Run.Values.Value.Inner: " + Error->Reason);
	const auto Ops = MakePropertyValueOps<int32>();
	Inner->SetValueLifecycle(Ops.ValueSize, Ops.ValueAlignment, Ops.InitializeValue, Ops.DestroyValue, Ops.CopyConstructValue, Ops.CopyAssignValue);
	ASSERT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_FALSE(Owner->GetFunctionLinkDiagnostic().HasError());
	EXPECT_FALSE(Function->GetLinkDiagnostic().HasError());
}

TEST(DFunctionReflectionTests, ClassLinkReportsTheFunctionWithAMissingNativeEntry)
{
	auto* Owner = MakeFunctionClass("Tests::DMissingNativeEntryDiagnostic");
	auto* Function = MakeFunction(Owner, "Run", EFunctionFlags::None, nullptr, nullptr);
	Owner->RegisterFunctions({Function});
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
	EXPECT_EQ(ToString(Owner->GetFunctionLinkDiagnostic()), "Tests::DMissingNativeEntryDiagnostic::Run: native function entry is missing");
	EXPECT_EQ(Function->GetLinkDiagnostic().Code, EFunctionLinkError::InvalidDescriptor);
	EXPECT_FALSE(Function->ArePropertiesLinked());
}

TEST(DFunctionReflectionTests, ParentLinkFailuresPreserveTheirOriginAndClearOnRetry)
{
	auto* Base = MakeFunctionClass("Tests::DDiagnosticCycleBase");
	auto* Derived = MakeFunctionClass("Tests::DDiagnosticCycleDerived", Base);
	Base->SetSuperStructBase(Derived);
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::InheritanceCycle);
	EXPECT_EQ(Derived->GetFunctionLinkDiagnostic().Code, EFunctionLinkError::InheritanceCycle);
	EXPECT_EQ(Derived->GetFunctionLinkDiagnostic().Reason, "class inheritance cycle");
	Base->SetSuperStructBase(DObject::StaticClass());
	ASSERT_EQ(Derived->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_FALSE(Base->GetFunctionLinkDiagnostic().HasError());
	EXPECT_FALSE(Derived->GetFunctionLinkDiagnostic().HasError());
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, SharedConstructionRejectsNativeStorageOverridesForMembersAndParameters)
{
	using namespace DurinCodeGen;
	auto* Owner = MakeFunctionClass("Tests::DInvalidNativeStorage");
	TNativePropertyParams<int32> Value("Value", 0);
	Value.Params.NativeValueOps = MakePropertyValueOps<int16>();
	const FPropertyParamsBase* Properties[]{&Value.Params};
	EXPECT_DEATH(ConstructProperties(*Owner, Properties, 1), "PropertyRegistration.InvalidNativeStorage.*DInvalidNativeStorage.*Value");
	struct FArguments { int32 Value; };
	Value.Params.Flags = EPropertyFlags::Parm;
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	const FFunctionParams Params{"Run", &Layout, EFunctionFlags::Native, "", "", "", nullptr};
	EXPECT_DEATH(ConstructDFunction(Owner, Params), "PropertyRegistration.InvalidNativeStorage.*Run.*Value");
}
#endif

TEST(DFunctionReflectionTests, PropertyRolesAreValidatedBeforeLinking)
{
	using namespace DurinCodeGen;
	struct FArguments { int32 Value, Result; };
	for (auto Flags : {EPropertyFlags::None, EPropertyFlags::Parm | EPropertyFlags::ConstParm,
		EPropertyFlags::Parm | EPropertyFlags::OutParm, EPropertyFlags::Parm | EPropertyFlags::ReferenceParm,
		EPropertyFlags::Parm | EPropertyFlags::ReturnParm})
	{
		auto* Owner = MakeFunctionClass("Tests::DInvalidFunctionRoles");
		TNativePropertyParams<int32> Value("Value", 0, Flags);
		const FPropertyParamsBase* Properties[]{&Value.Params};
		auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
		Owner->RegisterFunctions({MakeFunction(Owner, "Run", Layout)});
		EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::InvalidDescriptor);
	}
}

TEST(DFunctionReflectionTests, ReturnFieldDoesNotConflictWithAnInputNamedReturnValue)
{
	using namespace DurinCodeGen;
	auto* Owner = MakeFunctionClass("Tests::DReturnNameInput");
	struct FArguments { int32 Value, Result; };
	TNativePropertyParams<int32> Input("ReturnValue", 0, EPropertyFlags::Parm);
	TNativePropertyParams<int32> Return("ReturnValue", offsetof(FArguments, Result), EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
	const FPropertyParamsBase* Properties[]{&Input.Params, &Return.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 2);
	Owner->RegisterFunctions({MakeFunction(Owner, "Run", Layout)});
	EXPECT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_NE(Owner->FindFunctionByName("Run")->GetParameterProperty(0), Owner->FindFunctionByName("Run")->GetReturnProperty());
}

TEST(DFunctionReflectionTests, ParameterAggregateDestructionRunsAfterNativeException)
{
	auto* Class = MakeFunctionClass("Tests::DParameterLifecycleTarget");
	using namespace DurinCodeGen;
	GParameterConstructed = GParameterDestroyed = 0;
	TNativePropertyParams<std::string> Text("Text", STRUCT_OFFSET_UINT16(FObservedParameters, Text), EPropertyFlags::Parm | EPropertyFlags::ReferenceParm | EPropertyFlags::ConstParm);
	const FPropertyParamsBase* Properties[]{&Text.Params};
	auto Layout = MakeFunctionLayoutParams<FObservedParameters>(Properties, std::size(Properties));
	Class->RegisterFunctions({MakeFunction(Class, "Throw", Layout, +[](DObject&, FFunctionFrame& Frame, void*) {
			EXPECT_EQ(Frame.GetParameter<std::string>(0), "owned value");
			throw std::runtime_error("parameter body");
		})});
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "ParameterLifecycleTarget"));
	const auto* Function = Class->FindFunctionByName("Throw");
	{
		FFunctionParameterBuffer Buffer(*Function);
		Buffer.GetParameter<std::string>(0) = "owned value";
		EXPECT_EQ(GParameterConstructed, 1);
		EXPECT_THROW(Target->ProcessEvent(Function, Buffer.GetView()), std::runtime_error);
		EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
		EXPECT_EQ(GParameterDestroyed, 0);
	}
	EXPECT_EQ(GParameterDestroyed, 1);
}

TEST(DFunctionReflectionTests, LinkingRejectsAParameterTypeMismatchEvenWhenSizeAndAlignmentMatch)
{
	auto* Base = MakeFunctionClass("Tests::DTypedFunctionBase");
	auto* Derived = MakeFunctionClass("Tests::DTypedFunctionDerived", Base);
	struct FIntegerArguments { int32 Value; };
	struct FFloatArguments { float Value; };
	using namespace DurinCodeGen;
	TNativePropertyParams<int32> IntegerValue("Value", 0, EPropertyFlags::Parm);
	TNativePropertyParams<float> FloatValue("Renamed", 0, EPropertyFlags::Parm);
	const FPropertyParamsBase* IntegerProperties[]{&IntegerValue.Params};
	const FPropertyParamsBase* FloatProperties[]{&FloatValue.Params};
	auto Integer = MakeFunctionLayoutParams<FIntegerArguments>(IntegerProperties, 1);
	auto Float = MakeFunctionLayoutParams<FFloatArguments>(FloatProperties, 1);
	Derived->RegisterFunctions({MakeFunction(Derived, "Run", EFunctionFlags::Virtual, &Float)});
	Base->RegisterFunctions({MakeFunction(Base, "Run", EFunctionFlags::Virtual, &Integer)});
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::SignatureMismatch);
	EXPECT_EQ(Derived->GetFunctionLinkDiagnostic().OwnerClass, "Tests::DTypedFunctionDerived");
	EXPECT_EQ(Derived->GetFunctionLinkDiagnostic().PropertyPath, "Renamed");
	EXPECT_EQ(Derived->GetFunctionLinkDiagnostic().Reason, "native parameter type differs from the overridden function");
	EXPECT_FALSE(Derived->AreFunctionsLinked());
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, ParameterBufferRejectsTypedAccessWithTheWrongReflectedKind)
{
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Scale");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetParameter<float>(0);
	}()), "FunctionTypeMatches");
}
#endif

TEST(DFunctionReflectionTests, GeneratedObjectParametersAndReturnsPreserveIdentityAndNull)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "ObjectParameters"));
	TStrongObjectPtr<DParameterDerived> Value(NewObject<DParameterDerived>(nullptr, "ObjectParameterValue"));
	const auto* Raw = Target->GetClass()->FindFunctionByName("EchoObject");
	const auto* Wrapped = Target->GetClass()->FindFunctionByName("EchoWrapped");
	ASSERT_NE(Raw, nullptr);
	ASSERT_NE(Wrapped, nullptr);
	FFunctionParameterBuffer RawBuffer(*Raw), WrappedBuffer(*Wrapped);
	RawBuffer.GetParameter<DObject*>(0) = Value.Get();
	WrappedBuffer.GetParameter<TObjectPtr<DObject>>(0) = Value.Get();
	EXPECT_EQ(Target->ProcessEvent(Raw, RawBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Target->ProcessEvent(Wrapped, WrappedBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(RawBuffer.GetReturnValue<DObject*>(), Value.Get());
	EXPECT_EQ(WrappedBuffer.GetReturnValue<TObjectPtr<DObject>>().Get(), Value.Get());
	RawBuffer.GetParameter<DObject*>(0) = nullptr;
	WrappedBuffer.GetParameter<TObjectPtr<DObject>>(0) = nullptr;
	EXPECT_EQ(Target->ProcessEvent(Raw, RawBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Target->ProcessEvent(Wrapped, WrappedBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(RawBuffer.GetReturnValue<DObject*>(), nullptr);
	EXPECT_EQ(WrappedBuffer.GetReturnValue<TObjectPtr<DObject>>().Get(), nullptr);
}

TEST(DFunctionReflectionTests, GeneratedStructReferencesReturnAndVirtualDispatchUseNativeValueSemantics)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterDerived> Target(NewObject<DParameterDerived>(nullptr, "StructParameters"));
	const auto* Base = DParameterTarget::StaticClass()->FindFunctionByName("Transform");
	const auto* Derived = Target->GetClass()->FindFunctionByName("Transform");
	ASSERT_NE(Base, nullptr);
	ASSERT_NE(Derived, nullptr);
	EXPECT_EQ(Derived->GetSuperFunction(), Base);
	EXPECT_EQ(Derived->GetSuperStructBase(), Base);
	EXPECT_EQ(Derived->GetInheritanceSuper(), nullptr);
	const int32 Baseline = FFunctionPayload::Live;
	{
		FFunctionParameterBuffer Buffer(*Base);
		EXPECT_EQ(FFunctionPayload::Live, Baseline + 3);
		Buffer.GetParameter<FFunctionPayload>(0).Text = "input";
		EXPECT_EQ(Target->ProcessEvent(Base, Buffer.GetView()), EFunctionInvokeError::None);
		EXPECT_EQ(Buffer.GetParameter<FFunctionPayload>(0).Text, "input");
		EXPECT_EQ(Buffer.GetParameter<FFunctionPayload>(1).Text, "input derived");
		EXPECT_EQ(Buffer.GetReturnValue<FFunctionPayload>().Text, "input derived");
		EXPECT_EQ(FFunctionPayload::Live, Baseline + 3);
	}
	EXPECT_EQ(FFunctionPayload::Live, Baseline);
}

TEST(DFunctionReflectionTests, ObjectInputOutputReferencesAndIntrinsicStructReturnsUseTypedStorage)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "ObjectReferenceParameters"));
	TStrongObjectPtr<DObject> Previous(NewObject<DObject>(nullptr, "ObjectReferencePrevious"));
	const auto* Function = Target->GetClass()->FindFunctionByName("ReplaceObject");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	Buffer.GetParameter<DObject*>(0) = Previous.Get();
	Buffer.GetParameter<DObject*>(1) = Target.Get();
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetParameter<DObject*>(0), Target.Get());
	EXPECT_EQ(Buffer.GetReturnValue<DObject*>(), Previous.Get());
	const auto* Intrinsic = Target->GetClass()->FindFunctionByName("EchoTransform");
	ASSERT_NE(Intrinsic, nullptr);
	FFunctionParameterBuffer IntrinsicBuffer(*Intrinsic);
	IntrinsicBuffer.GetParameter<FTransform>(0).Translation = FVector3(1.0, 2.0, 3.0);
	EXPECT_EQ(Target->ProcessEvent(Intrinsic, IntrinsicBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(IntrinsicBuffer.GetReturnValue<FTransform>().Translation, FVector3(1.0, 2.0, 3.0));
}

TEST(DFunctionReflectionTests, NestedMapArrayStructParametersRoundTripAndDestroyEveryElement)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FPayloadMap = std::unordered_map<std::string, std::vector<FFunctionPayload>>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "ContainerParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("TransformMap");
	ASSERT_NE(Function, nullptr);
	const int32 Baseline = FFunctionPayload::Live;
	{
		FFunctionParameterBuffer Buffer(*Function);
		auto& Input = Buffer.GetParameter<FPayloadMap>(0);
		Input["input"].resize(2);
		Input["input"][0].Text = "first";
		Input["input"][1].Text = "second";
		for (int32 Index = 0; Index < 3; ++Index)
		{
			EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
			EXPECT_EQ(Buffer.GetParameter<FPayloadMap>(1).at("result")[0].Text, "first");
			EXPECT_EQ(Buffer.GetReturnValue<FPayloadMap>().at("result")[1].Text, "second");
			EXPECT_EQ(Input.size(), 1u);
			EXPECT_EQ(FFunctionPayload::Live, Baseline + 10);
		}
	}
	EXPECT_EQ(FFunctionPayload::Live, Baseline);
}

TEST(DFunctionReflectionTests, StructParameterExceptionRestoresFrameAndDestroysOwnedValues)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "ThrowStructParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("ThrowPayload");
	ASSERT_NE(Function, nullptr);
	const int32 Baseline = FFunctionPayload::Live;
	{
		FFunctionParameterBuffer Buffer(*Function);
		EXPECT_THROW(Target->ProcessEvent(Function, Buffer.GetView()), std::runtime_error);
		EXPECT_EQ(Buffer.GetParameter<FFunctionPayload>(0).Text, "changed");
		EXPECT_TRUE(Buffer.GetReturnValue<FFunctionPayload>().Text.empty());
		EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
		EXPECT_FALSE(IsGarbageCollectionDeferred());
	}
	EXPECT_EQ(FFunctionPayload::Live, Baseline);
	CollectGarbage(); // Also exercises removal of an exception-unwound buffer from GC roots.
}

TEST(DFunctionReflectionTests, LiveBuffersKeepNestedObjectInputsOutputsAndReturnsReachable)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FPayloadMap = std::unordered_map<std::string, std::vector<FFunctionPayload>>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "GcContainerParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("TransformMap");
	ASSERT_NE(Function, nullptr);
	const FObjectKey Key(NewObject<DObject>(nullptr, "NestedParameterOnlyObject"));
	{
		FFunctionParameterBuffer Buffer(*Function);
		auto& Input = Buffer.GetParameter<FPayloadMap>(0);
		Input["input"].resize(1);
		Input["input"][0].Objects.emplace_back(ResolveObjectKey(Key));
		CollectGarbage();
		ASSERT_TRUE(IsValid(ResolveObjectKey(Key)));
		ASSERT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
		Input.clear();
		Buffer.GetParameter<FPayloadMap>(1).clear();
		CollectGarbage();
		ASSERT_TRUE(IsValid(ResolveObjectKey(Key))); // Only the nested return slots retain it now.
		EXPECT_EQ(Buffer.GetReturnValue<FPayloadMap>().at("result")[0].Objects[0].Get(), ResolveObjectKey(Key));
	}
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(DFunctionReflectionTests, CachedParameterSchemaObservesLaterStructRegistrationAndPreservesRawObserverPolicy)
{
	using namespace DurinCodeGen;
	using FValues = std::vector<FDeferredFunctionPayload>;
	struct FArguments { FValues Values; };
	auto* Owner = MakeFunctionClass("Tests::DDeferredParameterSchema");
	TNativePropertyParams<FValues> Values("Values", 0, EPropertyFlags::Parm);
	const FPropertyParamsBase* Properties[]{&Values.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	auto* Function = MakeFunction(Owner, "Run", Layout);
	Owner->RegisterFunctions({Function});
	ASSERT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	{
		FFunctionParameterBuffer First(*Function), Second(*Function);
		First.GetParameter<FValues>(0).resize(1);
		CollectGarbage(); // Assemble an empty, provisional struct schema after the function cache is published.
		First.GetParameter<FValues>(0).clear();
		TNativePropertyParams<TObjectPtr<DObject>> Object("Object", offsetof(FDeferredFunctionPayload, Object));
		TNativePropertyParams<DObject*> Raw("Raw", offsetof(FDeferredFunctionPayload, Raw));
		const FPropertyParamsBase* Fields[]{&Object.Params, &Raw.Params};
		const FStructParams Params{&FDeferredFunctionPayload::StaticStruct, "Tests::FDeferredFunctionPayload", "FDeferredFunctionPayload",
			sizeof(FDeferredFunctionPayload), alignof(FDeferredFunctionPayload), Fields, std::size(Fields), &GetDStructOps<FDeferredFunctionPayload>()};
		ASSERT_EQ(ConstructDStruct(Params), FDeferredFunctionPayload::StaticStruct());
		const FObjectKey FirstKey(NewObject<DObject>(nullptr, "FirstCachedSchemaObject"));
		const FObjectKey SecondKey(NewObject<DObject>(nullptr, "SecondCachedSchemaObject"));
		const FObjectKey ObserverKey(NewObject<DObject>(nullptr, "CachedSchemaRawObserver"));
		First.GetParameter<FValues>(0).push_back({ResolveObjectKey(FirstKey), ResolveObjectKey(ObserverKey)});
		Second.GetParameter<FValues>(0).push_back({ResolveObjectKey(SecondKey), nullptr});
		for (int Pass = 0; Pass < 3; ++Pass)
		{
			CollectGarbage();
			ASSERT_NE(ResolveObjectKey(FirstKey), nullptr);
			ASSERT_NE(ResolveObjectKey(SecondKey), nullptr);
			EXPECT_EQ(ResolveObjectKey(ObserverKey), nullptr);
		}
		First.GetParameter<FValues>(0).clear();
		CollectGarbage();
		EXPECT_EQ(ResolveObjectKey(FirstKey), nullptr);
		ASSERT_NE(ResolveObjectKey(SecondKey), nullptr);
		Second.GetParameter<FValues>(0).clear();
		CollectGarbage();
		EXPECT_EQ(ResolveObjectKey(SecondKey), nullptr);
	}
}

TEST(DFunctionReflectionTests, CachedReturnSchemaRunsNativeStructCollectorsWithoutReflectedFields)
{
	using namespace DurinCodeGen;
	struct FArguments { FNativeReferenceFunctionPayload ReturnValue; };
	auto* Owner = MakeFunctionClass("Tests::DNativeCollectorReturnSchema");
	TNativePropertyParams<FNativeReferenceFunctionPayload> Result("ReturnValue", 0,
		EPropertyFlags::Parm | EPropertyFlags::ReturnParm | EPropertyFlags::OutParm);
	const FPropertyParamsBase* Properties[]{&Result.Params};
	auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
	auto* Function = MakeFunction(Owner, "Run", Layout);
	Owner->RegisterFunctions({Function});
	ASSERT_EQ(Owner->LinkFunctions(), EFunctionLinkError::None);
	const FObjectKey Key(NewObject<DObject>(nullptr, "NativeCollectorReturnOnlyObject"));
	{
		FFunctionParameterBuffer Buffer(*Function);
		Buffer.GetReturnValue<FNativeReferenceFunctionPayload>().Object = ResolveObjectKey(Key);
		for (int Pass = 0; Pass < 3; ++Pass)
		{
			CollectGarbage();
			ASSERT_NE(ResolveObjectKey(Key), nullptr);
		}
	}
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(DFunctionReflectionTests, BorrowedObjectCallDefersCollectionAndOwnedReturnSurvivesSafePoint)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "GcDuringFunction"));
	const auto* Function = Target->GetClass()->FindFunctionByName("CollectDuringCall");
	ASSERT_NE(Function, nullptr);
	const FObjectKey Key(NewObject<DObject>(nullptr, "BorrowedParameterOnlyObject"));
	{
		struct FArguments { std::vector<DObject*> Values, ReturnValue; } Arguments;
		Arguments.Values.push_back(ResolveObjectKey(Key));
		EXPECT_EQ(Target->ProcessEvent(Function, std::as_writable_bytes(std::span{&Arguments, 1})), EFunctionInvokeError::None);
		EXPECT_TRUE(IsGarbageCollectionRequested());
		EXPECT_FALSE(IsGarbageCollectionDeferred());
		ASSERT_TRUE(IsValid(ResolveObjectKey(Key)));
		EXPECT_EQ(Arguments.ReturnValue, Arguments.Values);
		// Copy the returned object into an owned buffer before permitting collection.
		FFunctionParameterBuffer Buffer(*Function);
		Buffer.GetReturnValue<std::vector<DObject*>>() = Arguments.ReturnValue;
		CollectGarbage();
		EXPECT_TRUE(IsValid(ResolveObjectKey(Key)));
	}
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(DFunctionReflectionTests, CompositeSignatureCompatibilityComparesRecursiveIdentities)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace DurinCodeGen;
	using namespace FunctionFixtures;
	auto Signature = []<typename T>() {
		struct FArguments { T Value; };
		TNativePropertyParams<T> Value("Value", 0, EPropertyFlags::Parm);
		const FPropertyParamsBase* Properties[]{&Value.Params};
		auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
		auto* Owner = MakeFunctionClass("Tests::DSignatureComparison");
		auto* Function = ConstructDFunction(Owner, {"Compare", &Layout, EFunctionFlags::Native, "", "", "", +[](DObject&, FFunctionFrame&, void*) {}});
		Owner->RegisterFunctions({Function});
		require(Owner->LinkFunctions() == EFunctionLinkError::None);
		return Function;
	};
	EXPECT_FALSE(Signature.operator()<std::vector<int32>>()->IsSignatureCompatibleWith(*Signature.operator()<std::vector<float>>()));
	EXPECT_FALSE(Signature.operator()<DObject*>()->IsSignatureCompatibleWith(*Signature.operator()<DParameterTarget*>()));
	EXPECT_FALSE(Signature.operator()<DObject*>()->IsSignatureCompatibleWith(*Signature.operator()<TObjectPtr<DObject>>()));
	EXPECT_FALSE(Signature.operator()<FFunctionPayload>()->IsSignatureCompatibleWith(*Signature.operator()<FOtherFunctionPayload>()));
	EXPECT_TRUE(Signature.operator()<std::vector<FFunctionPayload>>()->IsSignatureCompatibleWith(*Signature.operator()<std::vector<FFunctionPayload>>()));
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, TypedCompositeAccessRejectsSameSizeDifferentElementAndStructTypes)
{
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("EchoObjects");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetParameter<std::vector<FunctionFixtures::DParameterTarget*>>(0);
	}()), "FunctionTypeMatches");
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Transform");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetReturnValue<FunctionFixtures::FOtherFunctionPayload>();
	}()), "FunctionTypeMatches");
}
#endif

TEST(DFunctionReflectionTests, GeneratedEnumCallsPreserveEveryUnderlyingWidthAndSignedness)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "EnumParameters"));
	auto Check = [&]<typename T>(const char* Name, T Value) {
		const auto* Function = Target->GetClass()->FindFunctionByName(Name);
		ASSERT_NE(Function, nullptr);
		const auto* Enum = StaticEnum<T>();
		ASSERT_NE(Enum, nullptr);
		EXPECT_EQ(Enum->GetUnderlyingType(), DurinCodeGen::NativeEnumUnderlyingType<T>());
		EXPECT_EQ(Enum->GetUnderlyingSize(), sizeof(T));
		FFunctionParameterBuffer Buffer(*Function);
		Buffer.GetParameter<T>(0) = Value;
		EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
		EXPECT_EQ(Buffer.GetReturnValue<T>(), Value);
	};
	Check("EchoState", EFunctionState::High);
	Check("EchoState", static_cast<EFunctionState>(42));
	Check("EchoI8", EFunctionI8::Low);
	Check("EchoI16", EFunctionI16::Low);
	Check("EchoU16", EFunctionU16::High);
	Check("EchoI32", EFunctionI32::Low);
	Check("EchoU32", EFunctionU32::High);
	Check("EchoI64", EFunctionI64::Low);
	Check("EchoU64", EFunctionU64::High);
	Check("EchoPlain", PlainSeven);
}

TEST(DFunctionReflectionTests, NativeEnumRegistryRemovesRetiredResolvers)
{
	Testing::InitializeDObjectSystemForTests();
	enum class ETemporaryNativeEnum : uint8 { Value };
	EXPECT_EQ(StaticEnum<ETemporaryNativeEnum>(), nullptr);
	{
		DurinCodeGen::FNativeEnumRegistration Registration(typeid(ETemporaryNativeEnum), +[] { return StaticEnum<FunctionFixtures::EFunctionState>(); });
		EXPECT_EQ(StaticEnum<ETemporaryNativeEnum>(), StaticEnum<FunctionFixtures::EFunctionState>());
	}
	EXPECT_EQ(StaticEnum<ETemporaryNativeEnum>(), nullptr);
}

TEST(DFunctionReflectionTests, SoftReferenceCallsPreservePendingAndNullPathsWithoutLoading)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FSoft = TSoftObjectPtr<DObject>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "SoftParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("EchoSoft");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	EXPECT_TRUE(Buffer.GetParameter<FSoft>(0).IsNull());
	EXPECT_TRUE(Buffer.GetParameter<FSoft>(1).IsNull());
	EXPECT_TRUE(Buffer.GetReturnValue<FSoft>().IsNull());
	const auto Path = FunctionSoftPath("Pending");
	Buffer.GetParameter<FSoft>(0).SetPath(Path);
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetParameter<FSoft>(1).GetPath(), Path);
	EXPECT_EQ(Buffer.GetReturnValue<FSoft>().GetPath(), Path);
	EXPECT_EQ(Buffer.GetReturnValue<FSoft>().GetState(), ESoftObjectPtrState::Pending);
	EXPECT_EQ(Buffer.GetReturnValue<FSoft>().Get(), nullptr);
	Buffer.GetParameter<FSoft>(0).Reset();
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_TRUE(Buffer.GetParameter<FSoft>(1).IsNull());
	EXPECT_TRUE(Buffer.GetReturnValue<FSoft>().IsNull());
	const auto* Property = static_cast<const FSoftObjectProperty*>(Function->GetReturnProperty());
	EXPECT_EQ(Property->GetSoftObjectPtr(Buffer.GetView().data()), &Buffer.GetReturnValue<FSoft>().GetBase());
}

TEST(DFunctionReflectionTests, LoadedSoftReferencesInEnumMapsDoNotKeepAssetsAlive)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FSoftMap = std::unordered_map<EFunctionState, std::vector<TSoftObjectPtr<DObject>>>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "SoftMapParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("EchoSoftMap");
	ASSERT_NE(Function, nullptr);
	const auto Path = FunctionSoftPath("Loaded");
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionRefs/Loaded", PackagePath));
	auto* Package = NewObject<DPackage>(nullptr, "Loaded");
	Package->InitializeAssetPackage(PackagePath);
	TStrongObjectPtr<DObject> Asset(NewObject<DObject>(Package, "Loaded"));
	const FObjectKey Key(Asset.Get());
	FFunctionParameterBuffer Buffer(*Function);
	auto& Soft = Buffer.GetParameter<FSoftMap>(0)[EFunctionState::High].emplace_back();
	ASSERT_TRUE(Soft.TrySetObject(Asset.Get()));
	ASSERT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<FSoftMap>().at(EFunctionState::High)[0].Get(), Asset.Get());
	Buffer.GetParameter<FSoftMap>(0).clear();
	Asset.Reset();
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	const auto& Returned = Buffer.GetReturnValue<FSoftMap>().at(EFunctionState::High)[0];
	EXPECT_EQ(Returned.Get(), nullptr);
	EXPECT_EQ(Returned.GetPath(), Path);
	EXPECT_EQ(Returned.GetState(), ESoftObjectPtrState::Stale);
}

TEST(DFunctionReflectionTests, WeakReferenceInputsOutputsAndReturnsRemainNonowningAndPreserveStaleKeys)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FWeak = TWeakObjectPtr<DObject>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "WeakParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("EchoWeak");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	EXPECT_FALSE(Buffer.GetReturnValue<FWeak>().IsValid());
	TStrongObjectPtr<DObject> Value(NewObject<DObject>(nullptr, "WeakParameterOnlyObject"));
	const FObjectKey Key(Value.Get());
	Buffer.GetParameter<FWeak>(0) = Value.Get();
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetParameter<FWeak>(1).Get(), Value.Get());
	EXPECT_EQ(Buffer.GetReturnValue<FWeak>().Get(), Value.Get());
	const auto* Property = static_cast<const FWeakObjectProperty*>(Function->GetReturnProperty());
	EXPECT_EQ(Property->GetWeakObjectPtr(Buffer.GetView().data()), &Buffer.GetReturnValue<FWeak>().GetBase());
	Value.Reset();
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	EXPECT_FALSE(Buffer.GetParameter<FWeak>(0).IsValid());
	EXPECT_FALSE(Buffer.GetParameter<FWeak>(1).IsValid());
	EXPECT_EQ(Buffer.GetReturnValue<FWeak>().Get(), nullptr);
	EXPECT_EQ(Buffer.GetReturnValue<FWeak>().GetKey(), Key);
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<FWeak>().GetKey(), Key);
	Buffer.GetParameter<FWeak>(0).Reset();
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_TRUE(IsObjectKeyNull(Buffer.GetReturnValue<FWeak>().GetKey()));
}

TEST(DFunctionReflectionTests, WeakArrayReturnDoesNotBecomeAStrongReference)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using FWeakArray = std::vector<TWeakObjectPtr<DObject>>;
	TStrongObjectPtr<DParameterTarget> Target(NewObject<DParameterTarget>(nullptr, "WeakArrayParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("EchoWeakArray");
	ASSERT_NE(Function, nullptr);
	FFunctionParameterBuffer Buffer(*Function);
	const FObjectKey Key(NewObject<DObject>(nullptr, "WeakArrayOnlyObject"));
	Buffer.GetParameter<FWeakArray>(0).emplace_back(ResolveObjectKey(Key));
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView()), EFunctionInvokeError::None);
	Buffer.GetParameter<FWeakArray>(0).clear();
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	ASSERT_EQ(Buffer.GetReturnValue<FWeakArray>().size(), 1u);
	EXPECT_EQ(Buffer.GetReturnValue<FWeakArray>()[0].GetKey(), Key);
	EXPECT_EQ(Buffer.GetReturnValue<FWeakArray>()[0].Get(), nullptr);
}

TEST(DFunctionReflectionTests, EnumAndNonowningReferenceOverridesPreserveSlotsAndNativeDispatch)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterDerived> Target(NewObject<DParameterDerived>(nullptr, "ReferenceOverrides"));
	for (const char* Name : {"EchoState", "EchoSoft", "EchoWeak"})
	{
		const auto* Base = DParameterTarget::StaticClass()->FindFunctionByName(Name);
		const auto* Derived = Target->GetClass()->FindFunctionByName(Name);
		ASSERT_NE(Base, nullptr);
		ASSERT_NE(Derived, nullptr);
		EXPECT_EQ(Derived->GetSuperFunction(), Base);
	EXPECT_EQ(Derived->GetSuperStructBase(), Base);
	EXPECT_EQ(Derived->GetInheritanceSuper(), nullptr);
		EXPECT_TRUE(Derived->IsSignatureCompatibleWith(*Base));
		FFunctionParameterBuffer Buffer(*Base);
		if (std::string_view(Name) == "EchoState") Buffer.GetParameter<EFunctionState>(0) = EFunctionState::High;
		if (std::string_view(Name) == "EchoSoft") Buffer.GetParameter<TSoftObjectPtr<DObject>>(0).SetPath(FunctionSoftPath("Override"));
		if (std::string_view(Name) == "EchoWeak") Buffer.GetParameter<TWeakObjectPtr<DObject>>(0) = Target.Get();
		EXPECT_EQ(Target->ProcessEvent(Base, Buffer.GetView()), EFunctionInvokeError::None);
		if (std::string_view(Name) == "EchoState")
		{
			EXPECT_EQ(Buffer.GetParameter<EFunctionState>(1), EFunctionState::High);
			EXPECT_EQ(Buffer.GetReturnValue<EFunctionState>(), EFunctionState::Ready);
		}
		if (std::string_view(Name) == "EchoWeak") EXPECT_EQ(Buffer.GetReturnValue<TWeakObjectPtr<DObject>>().Get(), Target.Get());
	}
}

TEST(DFunctionReflectionTests, EnumAndReferenceSchemasRejectIdentityAndStorageMismatches)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using namespace DurinCodeGen;
	auto Signature = []<typename T>() {
		struct FArguments { T Value; };
		TNativePropertyParams<T> Value("Value", 0, EPropertyFlags::Parm);
		const FPropertyParamsBase* Properties[]{&Value.Params};
		auto Layout = MakeFunctionLayoutParams<FArguments>(Properties, 1);
		auto* Owner = MakeFunctionClass("Tests::DSignatureComparison");
		auto* Function = ConstructDFunction(Owner, {"Compare", &Layout, EFunctionFlags::Native, "", "", "", +[](DObject&, FFunctionFrame&, void*) {}});
		Owner->RegisterFunctions({Function});
		require(Owner->LinkFunctions() == EFunctionLinkError::None);
		return Function;
	};
	EXPECT_FALSE(Signature.operator()<EFunctionState>()->IsSignatureCompatibleWith(*Signature.operator()<EOtherFunctionState>()));
	EXPECT_FALSE(Signature.operator()<EFunctionState>()->IsSignatureCompatibleWith(*Signature.operator()<uint8>()));
	EXPECT_FALSE(Signature.operator()<TSoftObjectPtr<DObject>>()->IsSignatureCompatibleWith(*Signature.operator()<TWeakObjectPtr<DObject>>()));
	EXPECT_FALSE(Signature.operator()<TSoftObjectPtr<DObject>>()->IsSignatureCompatibleWith(*Signature.operator()<TSoftObjectPtr<DParameterTarget>>()));
	EXPECT_FALSE(Signature.operator()<TWeakObjectPtr<DObject>>()->IsSignatureCompatibleWith(*Signature.operator()<TWeakObjectPtr<DParameterTarget>>()));
	EXPECT_FALSE(Signature.operator()<TWeakObjectPtr<DObject>>()->IsSignatureCompatibleWith(*Signature.operator()<TObjectPtr<DObject>>()));
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, TypedEnumAndReferenceAccessRejectsWrongNativeIdentity)
{
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("EchoState");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetParameter<FunctionFixtures::EOtherFunctionState>(0);
	}()), "FunctionTypeMatches");
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("EchoSoft");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetReturnValue<TSoftObjectPtr<FunctionFixtures::DParameterTarget>>();
	}()), "FunctionTypeMatches");
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("EchoWeak");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetReturnValue<TWeakObjectPtr<FunctionFixtures::DParameterTarget>>();
	}()), "FunctionTypeMatches");
}
#endif

TEST(DFunctionReflectionTests, SharedPropertyTypeIdentityIgnoresFieldNamesAndOffsets)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace DurinCodeGen;
	const auto First = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<std::vector<int32>>("First", 0).Params);
	const auto Renamed = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<std::vector<int32>>("Renamed", 16).Params);
	const auto Different = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<std::vector<float>>("First", 0).Params);
	EXPECT_TRUE(ArePropertyTypesSame(First.get(), Renamed.get()));
	EXPECT_FALSE(ArePropertyTypesSame(First.get(), Different.get()));
	EXPECT_FALSE(ArePropertyTypesSame(First.get(), nullptr));
	EXPECT_FALSE(ArePropertyTypesSame(nullptr, nullptr));

	const FProperty Pair({}, "Pair", EObjectFlags::NoFlags, EPropertyFlags::None, 2, 0, sizeof(int32), EPropertyGenFlags::Int32, nullptr);
	const FProperty RenamedPair({}, "RenamedPair", EObjectFlags::NoFlags, EPropertyFlags::None, 2, 16, sizeof(int32), EPropertyGenFlags::Int32, nullptr);
	const FProperty Triple({}, "Triple", EObjectFlags::NoFlags, EPropertyFlags::None, 3, 0, sizeof(int32), EPropertyGenFlags::Int32, nullptr);
	const FProperty PaddedPair({}, "PaddedPair", EObjectFlags::NoFlags, EPropertyFlags::None, 2, 0, 2 * sizeof(int32), EPropertyGenFlags::Int32, nullptr);
	EXPECT_TRUE(ArePropertyTypesSame(&Pair, &RenamedPair));
	EXPECT_FALSE(ArePropertyTypesSame(&Pair, &Triple));
	EXPECT_FALSE(ArePropertyTypesSame(&Pair, &PaddedPair));
}

TEST(DFunctionReflectionTests, NestedPropertyTreeOutlivesGeneratedRecordsWithoutAnOwnershipCycle)
{
	using namespace Durin::FunctionFixtures;
	Testing::InitializeDObjectSystemForTests();
	using FValues = std::unordered_map<std::string, std::vector<FFunctionPayload>>;
	struct FArguments { FValues Values; };
	std::shared_ptr<const FProperty> Retained;
	std::weak_ptr<const FProperty> Weak;
	{
		DurinCodeGen::TNativePropertyParams<FValues> Record("Values", 0, EPropertyFlags::Parm);
		Retained = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), Record.Params);
		Weak = Retained;
	}
	ASSERT_FALSE(Weak.expired());
	const auto* Map = static_cast<const FMapProperty*>(Retained.get());
	ASSERT_NE(Map->GetKeyProp(), nullptr);
	ASSERT_NE(Map->GetValueProp(), nullptr);
	EXPECT_EQ(Map->GetKeyProp()->GetOwnerVariant().ToField(), Map);
	EXPECT_EQ(Map->GetValueProp()->GetOwnerVariant().ToField(), Map);
	const auto* Array = static_cast<const FArrayProperty*>(Map->GetValueProp());
	ASSERT_NE(Array->GetInner(), nullptr);
	EXPECT_EQ(Array->GetInner()->GetOwnerVariant().ToField(), Array);
	EXPECT_EQ(Array->GetInner()->GetKind(), DurinCodeGen::EPropertyGenFlags::Struct);
	{
		FReflectedValueStorage Storage;
		ASSERT_TRUE(Storage.DefaultConstruct(Retained.get(), 0));
	}
	Retained.reset();
	EXPECT_TRUE(Weak.expired());
}

TEST(DFunctionReflectionTests, FunctionMetadataIsAStructBaseWithLocallyOwnedParameterFields)
{
	using namespace Durin::FunctionFixtures;
	Testing::InitializeDObjectSystemForTests();
	static_assert(std::is_base_of_v<DStructBase, DFunction>);
	const auto* Function = DParameterTarget::StaticClass()->FindFunctionByName("Combine");
	ASSERT_NE(Function, nullptr);
	EXPECT_EQ(Cast<DFunction>(Function), Function);
	EXPECT_TRUE(IsValid(Function));
	EXPECT_EQ(Function->GetOuter(), Function->GetOuterDClass());
	size_t Count = 0;
	Function->ForEachProperty([&](FProperty* Property) {
		++Count;
		EXPECT_EQ(Property->GetOwnerVariant().ToDObject(), Function);
		EXPECT_TRUE(Property->HasAnyPropertyFlags(EPropertyFlags::Parm));
	});
	EXPECT_EQ(Count, Function->GetNumParms());
	ASSERT_NE(Function->GetReturnProperty(), nullptr);
	EXPECT_TRUE(Function->GetReturnProperty()->HasAnyPropertyFlags(EPropertyFlags::ReturnParm));
	EXPECT_EQ(Function->GetReturnValueOffset(), Function->GetReturnProperty()->GetOffset());
	EXPECT_TRUE(Function->GetParameterProperty(1)->HasAnyPropertyFlags(EPropertyFlags::ConstParm));
	EXPECT_TRUE(Function->GetParameterProperty(2)->HasAnyPropertyFlags(EPropertyFlags::OutParm));
	EXPECT_EQ(Function->GetSuperStructBase(), nullptr);
	CollectGarbage();
	EXPECT_TRUE(IsValid(Function));
}

#if GTEST_HAS_DEATH_TEST
TEST(DFunctionReflectionTests, OwnedPropertyCannotBeReparentedToAnotherMetadataScope)
{
	Testing::InitializeDObjectSystemForTests();
	auto* First = MakeFunctionClass("Tests::DFirstPropertyScope");
	auto* Second = MakeFunctionClass("Tests::DSecondPropertyScope");
	auto Property = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<int32>("Value", 0).Params);
	EXPECT_EQ(First->AddOwnedProperty(Property), Property.get());
	EXPECT_EQ(Property->GetOwnerVariant().ToDObject(), First);
	EXPECT_DEATH(Second->AddOwnedProperty(Property), "already belongs");
	EXPECT_DEATH(Property->SetOwner(FFieldVariant(Second)), "already belongs");
	auto Unowned = DurinCodeGen::ConstructProperty(FFieldVariant(nullptr), DurinCodeGen::TNativePropertyParams<int32>("Unowned", 0).Params);
	EXPECT_DEATH(Unowned->SetOwner(FFieldVariant(Unowned.get())), "ownership cycle");
}
#endif
