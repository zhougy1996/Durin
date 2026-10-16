#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/FunctionParameters.h"
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

	auto MakeFunction(DClass* Owner, const char* Name, EFunctionFlags Flags = EFunctionFlags::Virtual) -> FFunction
	{
		return FFunction(Owner, Name, FFunctionSignature{}, Flags | EFunctionFlags::Native,
			"Actions", Name, "", +[](DObject&, FFunctionFrame&, void*) {});
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

TEST(FFunctionReflectionTests, RecursivePropertyResolutionInstallsFunctionsOnlyOnce)
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

TEST(FFunctionReflectionTests, LinkingResolvesLateParentsAndCachesEffectiveSlots)
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
	EXPECT_TRUE(Function->GetSignature().IsVoidNoArgs());
	EXPECT_EQ(Derived->FindFunctionByName("Inherited", false), nullptr);
	EXPECT_EQ(Derived->FindFunctionByName("Missing"), nullptr);
	std::vector<FName> Names;
	Derived->ForEachFunction([&](const FFunction& Entry) { Names.push_back(Entry.GetName()); });
	EXPECT_EQ(Names, (std::vector<FName>{"Inherited", "Run", "Local"}));
	Names.clear();
	Derived->ForEachFunction([&](const FFunction& Entry) { Names.push_back(Entry.GetName()); }, false);
	EXPECT_EQ(Names, (std::vector<FName>{"Run", "Local"}));
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Derived->FindFunctionByName("Run"), Function);
}

TEST(FFunctionReflectionTests, LinkingRejectsNonVirtualHidingRegardlessOfRegistrationOrder)
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

TEST(FFunctionReflectionTests, LinkingRejectsFinalOverrideThroughAnIntermediateClass)
{
	auto* Base = MakeFunctionClass("Tests::DFinalFunctionBase");
	auto* Middle = MakeFunctionClass("Tests::DFinalFunctionMiddle", Base);
	auto* Derived = MakeFunctionClass("Tests::DFinalFunctionDerived", Middle);
	Derived->RegisterFunctions({MakeFunction(Derived, "Run")});
	Base->RegisterFunctions({MakeFunction(Base, "Run", EFunctionFlags::Virtual | EFunctionFlags::Final)});
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::FinalOverride);
	EXPECT_FALSE(Derived->AreFunctionsLinked());
}

TEST(FFunctionReflectionTests, InvocationRequiresLinkedMetadataAndALiveCompatibleTarget)
{
	auto* Class = MakeFunctionClass("Tests::DFunctionInvocationTarget");
	GFunctionInvocations = 0;
	FFunction Function(Class, "Run", FFunctionSignature{}, EFunctionFlags::Native,
		"Actions", "Run", "", +[](DObject&, FFunctionFrame&, void*) { ++GFunctionInvocations; });
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "FunctionInvocationTarget"));
	ASSERT_TRUE(Target);
	EXPECT_EQ(Target->ProcessEvent(&Function, {}), EFunctionInvokeError::UnlinkedFunction);
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

TEST(FFunctionReflectionTests, NestedProcessEventBuildsAndRestoresNativeFrames)
{
	auto* Class = MakeFunctionClass("Tests::DNestedFunctionTarget");
	GFunctionInvocations = 0;
	Class->RegisterFunctions({
		FFunction(Class, "Outer", FFunctionSignature{}, EFunctionFlags::Native,
			"Actions", "Outer", "", +[](DObject& Target, FFunctionFrame& Frame, void* Result) {
				EXPECT_EQ(&Frame.GetObject(), &Target);
				EXPECT_EQ(Frame.GetFunction().GetName(), FName("Outer"));
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
			}),
		FFunction(Class, "Inner", FFunctionSignature{}, EFunctionFlags::Native,
			"Actions", "Inner", "", +[](DObject& Target, FFunctionFrame& Frame, void* Result) {
				ASSERT_NE(Frame.GetPreviousFrame(), nullptr);
				EXPECT_EQ(&Frame.GetObject(), &Target);
				EXPECT_EQ(Frame.GetFunction().GetName(), FName("Inner"));
				EXPECT_EQ(Frame.GetPreviousFrame()->GetFunction().GetName(), FName("Outer"));
				EXPECT_EQ(&Frame.GetPreviousFrame()->GetObject(), &Target);
				EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
				EXPECT_EQ(Result, nullptr);
				++GFunctionInvocations;
			})});
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "NestedFunctionTarget"));
	ASSERT_TRUE(Target);
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_EQ(Target->ProcessEvent(Class->FindFunctionByName("Outer"), {}), EFunctionInvokeError::None);
	EXPECT_EQ(GFunctionInvocations, 1);
	EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
}

TEST(FFunctionReflectionTests, NativeExceptionUnwindsTheCallFrameWithoutConvertingDomainFailure)
{
	auto* Class = MakeFunctionClass("Tests::DThrowingFunctionTarget");
	Class->RegisterFunctions({FFunction(Class, "Throw", FFunctionSignature{}, EFunctionFlags::Native,
		"Actions", "Throw", "", +[](DObject&, FFunctionFrame& Frame, void*) {
			EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), &Frame);
			throw std::runtime_error("native function body");
		})});
	TStrongObjectPtr<DObject> Target(NewObject(Class, nullptr, "ThrowingFunctionTarget"));
	ASSERT_TRUE(Target);
	ASSERT_EQ(Class->LinkFunctions(), EFunctionLinkError::None);
	EXPECT_THROW(Target->ProcessEvent(Class->FindFunctionByName("Throw"), {}), std::runtime_error);
	EXPECT_EQ(FFunctionFrame::GetCurrentFrame(), nullptr);
}

TEST(FFunctionReflectionTests, GeneratedStringArgumentsReferenceOutputAndReturnUseInitializedStorage)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "StringParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Combine");
	ASSERT_NE(Function, nullptr);
	const auto& Signature = Function->GetSignature();
	ASSERT_EQ(Signature.Parameters.size(), 3u);
	EXPECT_TRUE(Function->HasAnyFlags(EFunctionFlags::Const));
	EXPECT_EQ(Signature.Parameters[1].Passing, EFunctionParameterPassing::ConstReference);
	EXPECT_EQ(Signature.Parameters[2].Passing, EFunctionParameterPassing::Reference);
	ASSERT_NE(Signature.ReturnProperty, nullptr);
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
	EXPECT_EQ(Target->ProcessEvent(Function, Buffer.GetView().first(Signature.ParmsSize - 1)), EFunctionInvokeError::InvalidParameters);
	std::vector<std::byte> Misaligned(Signature.ParmsSize + Signature.ParmsAlignment);
	size_t Offset = 0;
	while (reinterpret_cast<uintptr_t>(Misaligned.data() + Offset) % Signature.ParmsAlignment == 0) ++Offset;
	EXPECT_EQ(Target->ProcessEvent(Function, {Misaligned.data() + Offset, Signature.ParmsSize}), EFunctionInvokeError::InvalidParameters);
}

TEST(FFunctionReflectionTests, GeneratedThunkAcceptsBorrowedCompilerLaidOutParameterAggregate)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "BorrowedParameters"));
	const auto* Function = Target->GetClass()->FindFunctionByName("Combine");
	ASSERT_NE(Function, nullptr);
	struct FArguments { int32 Value; std::string Prefix, Output, ReturnValue; } Arguments{6, "raw=", {}, {}};
	ASSERT_EQ(sizeof(Arguments), Function->GetSignature().ParmsSize);
	ASSERT_EQ(offsetof(FArguments, ReturnValue), Function->GetSignature().ReturnProperty->GetOffset());
	EXPECT_EQ(Target->ProcessEvent(Function, std::as_writable_bytes(std::span(&Arguments, 1))), EFunctionInvokeError::None);
	EXPECT_EQ(Arguments.Output, "raw=6");
	EXPECT_EQ(Arguments.ReturnValue, "raw=6!");
}

TEST(FFunctionReflectionTests, GeneratedReturnOnlyReferenceOnlyAndMixedAlignmentSignaturesDispatch)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterTarget> Target(NewObject<FunctionFixtures::DParameterTarget>(nullptr, "MixedParameters"));
	const auto* Read = Target->GetClass()->FindFunctionByName("ReadValue");
	ASSERT_NE(Read, nullptr);
	EXPECT_FALSE(Read->GetSignature().IsVoidNoArgs());
	FFunctionParameterBuffer ReadBuffer(*Read);
	EXPECT_EQ(Target->ProcessEvent(Read, ReadBuffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(ReadBuffer.GetReturnValue<int32>(), 17);
	const auto* Update = Target->GetClass()->FindFunctionByName("Update");
	ASSERT_NE(Update, nullptr);
	EXPECT_EQ(Update->GetSignature().ReturnProperty, nullptr);
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

TEST(FFunctionReflectionTests, GeneratedNameAndGuidValuesRoundTrip)
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

TEST(FFunctionReflectionTests, ParameterizedVirtualOverridePreservesSlotAndIgnoresParameterNames)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<FunctionFixtures::DParameterDerived> Target(NewObject<FunctionFixtures::DParameterDerived>(nullptr, "OverrideParameters"));
	const auto* Base = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Scale");
	const auto* Derived = Target->GetClass()->FindFunctionByName("Scale");
	ASSERT_NE(Base, nullptr);
	ASSERT_NE(Derived, nullptr);
	EXPECT_EQ(Derived->GetSuperFunction(), Base);
	EXPECT_TRUE(Derived->IsSignatureCompatibleWith(*Base));
	FFunctionParameterBuffer Buffer(*Base);
	Buffer.GetParameter<int32>(0) = 7;
	EXPECT_EQ(Target->ProcessEvent(Derived, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 21);
	EXPECT_EQ(Target->ProcessEvent(Base, Buffer.GetView()), EFunctionInvokeError::None);
	EXPECT_EQ(Buffer.GetReturnValue<int32>(), 21);
}

TEST(FFunctionReflectionTests, LayoutValidationRejectsOverlapMissingLifecycleAndInvalidBounds)
{
	Testing::InitializeDObjectSystemForTests();
	struct FArguments { int32 Value, Result; };
	using namespace DurinCodeGen;
	auto Valid = MakeFunctionSignature<FArguments>({{MakeFunctionProperty<int32>("Value", 0), EFunctionParameterPassing::Value}},
		MakeFunctionProperty<int32>("ReturnValue", offsetof(FArguments, Result)));
	EXPECT_TRUE(Valid.IsValid());
	auto Invalid = Valid;
	Invalid.ReturnProperty = MakeFunctionProperty<int32>("ReturnValue", 0);
	EXPECT_FALSE(Invalid.IsValid());
	EXPECT_FALSE(Invalid.IsCompatibleWith(Valid));
	EXPECT_FALSE(Valid.IsCompatibleWith(Invalid));
	Invalid = Valid;
	Invalid.Initialize = nullptr;
	EXPECT_FALSE(Invalid.IsValid());
	EXPECT_FALSE(Invalid.IsCompatibleWith(Valid));
	EXPECT_FALSE(Valid.IsCompatibleWith(Invalid));
	Invalid = Valid;
	Invalid.ParmsSize = 1;
	EXPECT_FALSE(Invalid.IsValid());
	EXPECT_FALSE(Invalid.IsCompatibleWith(Valid));
	EXPECT_FALSE(Valid.IsCompatibleWith(Invalid));
	Invalid = Valid;
	Invalid.ParmsAlignment = 3;
	EXPECT_FALSE(Invalid.IsValid());
	EXPECT_FALSE(Invalid.IsCompatibleWith(Valid));
	EXPECT_FALSE(Valid.IsCompatibleWith(Invalid));
}

TEST(FFunctionReflectionTests, ParameterAggregateDestructionRunsAfterNativeException)
{
	auto* Class = MakeFunctionClass("Tests::DParameterLifecycleTarget");
	using namespace DurinCodeGen;
	GParameterConstructed = GParameterDestroyed = 0;
	auto Signature = MakeFunctionSignature<FObservedParameters>({{
		MakeFunctionProperty<std::string>("Text", STRUCT_OFFSET_UINT16(FObservedParameters, Text)), EFunctionParameterPassing::ConstReference}});
	Class->RegisterFunctions({FFunction(Class, "Throw", std::move(Signature), EFunctionFlags::Native,
		"Actions", "Throw", "", +[](DObject&, FFunctionFrame& Frame, void*) {
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

TEST(FFunctionReflectionTests, LinkingRejectsAParameterTypeMismatchEvenWhenSizeAndAlignmentMatch)
{
	auto* Base = MakeFunctionClass("Tests::DTypedFunctionBase");
	auto* Derived = MakeFunctionClass("Tests::DTypedFunctionDerived", Base);
	struct FIntegerArguments { int32 Value; };
	struct FFloatArguments { float Value; };
	using namespace DurinCodeGen;
	auto Integer = MakeFunctionSignature<FIntegerArguments>({{MakeFunctionProperty<int32>("Value", 0), EFunctionParameterPassing::Value}});
	auto Float = MakeFunctionSignature<FFloatArguments>({{MakeFunctionProperty<float>("Renamed", 0), EFunctionParameterPassing::Value}});
	EXPECT_FALSE(Integer.IsCompatibleWith(Float));
	const auto Flags = EFunctionFlags::Native | EFunctionFlags::Virtual;
	const auto Native = +[](DObject&, FFunctionFrame&, void*) {};
	Derived->RegisterFunctions({FFunction(Derived, "Run", std::move(Float), Flags, "", "", "", Native)});
	Base->RegisterFunctions({FFunction(Base, "Run", std::move(Integer), Flags, "", "", "", Native)});
	EXPECT_EQ(Derived->LinkFunctions(), EFunctionLinkError::SignatureMismatch);
	EXPECT_FALSE(Derived->AreFunctionsLinked());
}

#if GTEST_HAS_DEATH_TEST
TEST(FFunctionReflectionTests, ParameterBufferRejectsTypedAccessWithTheWrongReflectedKind)
{
	EXPECT_DEATH(([] {
		Testing::InitializeDObjectSystemForTests();
		const auto* Function = FunctionFixtures::DParameterTarget::StaticClass()->FindFunctionByName("Scale");
		FFunctionParameterBuffer Buffer(*Function);
		(void)Buffer.GetParameter<float>(0);
	}()), "FunctionTypeMatches");
}
#endif

TEST(FFunctionReflectionTests, GeneratedObjectParametersAndReturnsPreserveIdentityAndNull)
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

TEST(FFunctionReflectionTests, GeneratedStructReferencesReturnAndVirtualDispatchUseNativeValueSemantics)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	TStrongObjectPtr<DParameterDerived> Target(NewObject<DParameterDerived>(nullptr, "StructParameters"));
	const auto* Base = DParameterTarget::StaticClass()->FindFunctionByName("Transform");
	const auto* Derived = Target->GetClass()->FindFunctionByName("Transform");
	ASSERT_NE(Base, nullptr);
	ASSERT_NE(Derived, nullptr);
	EXPECT_EQ(Derived->GetSuperFunction(), Base);
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

TEST(FFunctionReflectionTests, ObjectInputOutputReferencesAndIntrinsicStructReturnsUseTypedStorage)
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

TEST(FFunctionReflectionTests, NestedMapArrayStructParametersRoundTripAndDestroyEveryElement)
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

TEST(FFunctionReflectionTests, StructParameterExceptionRestoresFrameAndDestroysOwnedValues)
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

TEST(FFunctionReflectionTests, LiveBuffersKeepNestedObjectInputsOutputsAndReturnsReachable)
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

TEST(FFunctionReflectionTests, BorrowedObjectCallDefersCollectionAndOwnedReturnSurvivesSafePoint)
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

TEST(FFunctionReflectionTests, CompositeSignatureCompatibilityComparesRecursiveIdentities)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace DurinCodeGen;
	using namespace FunctionFixtures;
	auto Signature = []<typename T>() {
		struct FArguments { T Value; };
		return MakeFunctionSignature<FArguments>({{MakeFunctionProperty<T>("Value", 0), EFunctionParameterPassing::Value}});
	};
	EXPECT_FALSE(Signature.operator()<std::vector<int32>>().IsCompatibleWith(Signature.operator()<std::vector<float>>()));
	EXPECT_FALSE(Signature.operator()<DObject*>().IsCompatibleWith(Signature.operator()<DParameterTarget*>()));
	EXPECT_FALSE(Signature.operator()<DObject*>().IsCompatibleWith(Signature.operator()<TObjectPtr<DObject>>()));
	EXPECT_FALSE(Signature.operator()<FFunctionPayload>().IsCompatibleWith(Signature.operator()<FOtherFunctionPayload>()));
	EXPECT_TRUE(Signature.operator()<std::vector<FFunctionPayload>>().IsCompatibleWith(Signature.operator()<std::vector<FFunctionPayload>>()));
}

#if GTEST_HAS_DEATH_TEST
TEST(FFunctionReflectionTests, TypedCompositeAccessRejectsSameSizeDifferentElementAndStructTypes)
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

TEST(FFunctionReflectionTests, GeneratedEnumCallsPreserveEveryUnderlyingWidthAndSignedness)
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

TEST(FFunctionReflectionTests, NativeEnumRegistryRemovesRetiredResolvers)
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

TEST(FFunctionReflectionTests, SoftReferenceCallsPreservePendingAndNullPathsWithoutLoading)
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
	const auto* Property = static_cast<const FSoftObjectProperty*>(Function->GetSignature().ReturnProperty.get());
	EXPECT_EQ(Property->GetSoftObjectPtr(Buffer.GetView().data()), &Buffer.GetReturnValue<FSoft>().GetBase());
}

TEST(FFunctionReflectionTests, LoadedSoftReferencesInEnumMapsDoNotKeepAssetsAlive)
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

TEST(FFunctionReflectionTests, WeakReferenceInputsOutputsAndReturnsRemainNonowningAndPreserveStaleKeys)
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
	const auto* Property = static_cast<const FWeakObjectProperty*>(Function->GetSignature().ReturnProperty.get());
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

TEST(FFunctionReflectionTests, WeakArrayReturnDoesNotBecomeAStrongReference)
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

TEST(FFunctionReflectionTests, EnumAndNonowningReferenceOverridesPreserveSlotsAndNativeDispatch)
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

TEST(FFunctionReflectionTests, EnumAndReferenceSchemasRejectIdentityAndStorageMismatches)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace FunctionFixtures;
	using namespace DurinCodeGen;
	auto Signature = []<typename T>() {
		struct FArguments { T Value; };
		return MakeFunctionSignature<FArguments>({{MakeFunctionProperty<T>("Value", 0), EFunctionParameterPassing::Value}});
	};
	EXPECT_FALSE(Signature.operator()<EFunctionState>().IsCompatibleWith(Signature.operator()<EOtherFunctionState>()));
	EXPECT_FALSE(Signature.operator()<EFunctionState>().IsCompatibleWith(Signature.operator()<uint8>()));
	EXPECT_FALSE(Signature.operator()<TSoftObjectPtr<DObject>>().IsCompatibleWith(Signature.operator()<TWeakObjectPtr<DObject>>()));
	EXPECT_FALSE(Signature.operator()<TSoftObjectPtr<DObject>>().IsCompatibleWith(Signature.operator()<TSoftObjectPtr<DParameterTarget>>()));
	EXPECT_FALSE(Signature.operator()<TWeakObjectPtr<DObject>>().IsCompatibleWith(Signature.operator()<TWeakObjectPtr<DParameterTarget>>()));
	EXPECT_FALSE(Signature.operator()<TWeakObjectPtr<DObject>>().IsCompatibleWith(Signature.operator()<TObjectPtr<DObject>>()));
}

#if GTEST_HAS_DEATH_TEST
TEST(FFunctionReflectionTests, TypedEnumAndReferenceAccessRejectsWrongNativeIdentity)
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

TEST(FFunctionReflectionTests, SharedPropertyTypeIdentityIgnoresFieldNamesAndOffsets)
{
	Testing::InitializeDObjectSystemForTests();
	using namespace DurinCodeGen;
	const auto First = MakeFunctionProperty<std::vector<int32>>("First", 0);
	const auto Renamed = MakeFunctionProperty<std::vector<int32>>("Renamed", 16);
	const auto Different = MakeFunctionProperty<std::vector<float>>("First", 0);
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

TEST(FFunctionReflectionTests, DetachedNestedPropertyTreeOutlivesSignaturesWithoutAnOwnershipCycle)
{
	using namespace Durin::FunctionFixtures;
	Testing::InitializeDObjectSystemForTests();
	using FValues = std::unordered_map<std::string, std::vector<FFunctionPayload>>;
	struct FArguments { FValues Values; };
	std::shared_ptr<const FProperty> Retained;
	std::weak_ptr<const FProperty> Weak;
	{
		auto Signature = DurinCodeGen::MakeFunctionSignature<FArguments>({{
			DurinCodeGen::MakeFunctionProperty<FValues>("Values", 0), EFunctionParameterPassing::Value}});
		ASSERT_TRUE(Signature.IsValid());
		auto Copy = Signature;
		Retained = Copy.Parameters[0].Property;
		Weak = Retained;
	}
	ASSERT_FALSE(Weak.expired());
	const auto* Map = static_cast<const FMapProperty*>(Retained.get());
	ASSERT_NE(Map->GetKeyProp(), nullptr);
	ASSERT_NE(Map->GetValueProp(), nullptr);
	EXPECT_EQ(Map->GetKeyProp()->Owner.ToField(), Map);
	EXPECT_EQ(Map->GetValueProp()->Owner.ToField(), Map);
	const auto* Array = static_cast<const FArrayProperty*>(Map->GetValueProp());
	ASSERT_NE(Array->GetInner(), nullptr);
	EXPECT_EQ(Array->GetInner()->Owner.ToField(), Array);
	EXPECT_EQ(Array->GetInner()->GetKind(), DurinCodeGen::EPropertyGenFlags::Struct);
	{
		FReflectedValueStorage Storage;
		ASSERT_TRUE(Storage.DefaultConstruct(Retained.get(), 0));
	}
	Retained.reset();
	EXPECT_TRUE(Weak.expired());
}
