#include "PythonRuntime.h"
#include "PythonReflectionTestTypes.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorToolService.h"
#include "Tools/EditorScriptCall.h"
#include "Modules/ModuleManager.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "DObject/Package.h"
#include "DObject/AssetPath.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Transactions/Transactor.h"
#include "Components/SceneComponent.h"
#include "Components/CameraComponent.h"
#include "Actors/EditorActorSubsystem.h"
#include "Actors/CameraActor.h"
#include "PropertyEditor/PropertyEditing.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace
{
    using namespace Durin;
    using namespace Durin::PythonFixtures;
    class PythonReflectionTests : public testing::Test
    {
    protected:
        FPythonRuntime Runtime;
        TStrongObjectPtr<DEditorEngine> Host;
        DEditorToolSubsystem* Tools = nullptr;
        std::unique_ptr<FEditorSubsystemRegistration> First, Second;
        uint64 Session = 0;
        TStrongObjectPtr<DPackage> ScenePackage;
        DLevel* EditLevel = nullptr;
        AReflectionActor* EditActor = nullptr;
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            ASSERT_TRUE(FModuleManager::Get().LoadModule("Engine"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinEd"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinPython"));
            First = Register(DReflectionService::StaticClass()); Second = Register(DSecondaryService::StaticClass());
            Host = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
            ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host)); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(Tools, nullptr);
            const auto Started = Runtime.Start(); ASSERT_TRUE(Started) << Started.error();
            const auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        }
        auto TearDown() -> void override
        {
            AReflectionLifecycleActor::ConstructCallback = {};
            AReflectionLifecycleActor::LoadCallback = {};
            Runtime.Stop();
            if (Host) { Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
            First.reset(); Second.reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
            if (ScenePackage) { MarkObjectHierarchyAsGarbage(ScenePackage.Get()); ScenePackage.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone); }
        }
        static auto Register(DClass* Type) -> std::unique_ptr<FEditorSubsystemRegistration>
        { return std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = Type, .Provider = "DurinPython"}); }
        auto Run(std::string Source, uint64 InSession = 0, DEditorToolSubsystem* Pump = nullptr) -> FPythonJobResult
        {
            const auto Submitted = Runtime.RunString(InSession ? InSession : Session, "import durin, builtins\n" + Source);
            EXPECT_TRUE(Submitted); if (!Submitted) return {.State = EPythonJobState::Failed, .Error = Submitted.error()};
            for (int Index = 0; Index < 100; ++Index)
            {
                (Pump ? Pump : Tools)->TickScripts();
                const auto Result = Runtime.GetJobResult(*Submitted);
                if (Result && Result->State != EPythonJobState::Queued && Result->State != EPythonJobState::Running) return *Result;
            }
            ADD_FAILURE() << "Script did not settle"; return {.State = EPythonJobState::Failed};
        }
        auto Pass(std::string Source) -> void
        { const auto Result = Run(std::move(Source)); EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error; }
        auto Save() -> void
        { Pass("builtins.proxy = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)\nbuiltins.cached = builtins.proxy.get_name\nbuiltins.identity = hash(builtins.proxy)"); }
        auto PrepareScene(EPythonEditPolicy Policy = EPythonEditPolicy::NonDocumentReadWrite, bool WithActor = true) -> void
        {
            static const bool Registered = [] {
                const auto Root = Testing::GetTestWorkDirectory() / "PythonReflection";
                std::filesystem::create_directories(Root);
                Testing::RegisterMountPointForTests("/PythonReflectionTests/", Root.generic_string() + "/");
                return true;
            }();
            (void)Registered;
            static uint64 NextId = 1;
            FPackagePath Path;
            ASSERT_TRUE(FPackagePath::TryCreate(std::format("/PythonReflectionTests/Scene_{}", NextId++), Path));
            ScenePackage = NewObject<DPackage>(nullptr, Path.GetPackageName());
            ScenePackage->InitializeAssetPackage(Path);
            auto* World = NewObject<DWorld>(Host.Get(), "PropertyWorld"); World->SetWorldType(EWorldType::Editor);
            ASSERT_NO_THROW(World->InitializeSubsystems());
            EditLevel = NewObject<DLevel>(ScenePackage.Get(), "PropertyLevel"); ASSERT_TRUE(World->SetCurrentLevel(EditLevel));
            FEditorEngineTestAccess::SetEditorWorld(*Host, World);
            if (WithActor) { EditActor = EditLevel->SpawnActor<AReflectionActor>("Editable"); ASSERT_NE(EditActor, nullptr); }
            Host->GetEditorSubsystem<DReflectionService>()->EditTarget = EditActor;
            Runtime.CloseSession(Session);
            const auto Created = Runtime.CreateSession(Tools->GetService(), EToolSource::External, {}, {.EditPolicy = Policy});
            ASSERT_TRUE(Created); Session = *Created;
            ScenePackage->ClearDirty();
        }
        auto PrepareActorScene(EPythonEditPolicy Policy = EPythonEditPolicy::NonDocumentReadWrite) -> void
        {
            PrepareScene(Policy);
            ASSERT_NE(EditLevel->SpawnActor<ACameraActor>("Seed"), nullptr);
            ScenePackage->ClearDirty();
        }
        auto ActorPass(std::string Source) -> void
        { Pass("actors = durin.get_editor_subsystem(durin.types.Durin.EditorActorSubsystem)\nlevel = actors.GetEditorLevel()\n" + Source); }
    };
    TEST_F(PythonReflectionTests, InheritedScriptTypesPublishWithoutInstancesAndExclusionsWin)
    {
        EXPECT_FALSE(DInheritedScriptType::StaticClass()->GetScriptExposure().Has(EScriptExposure::Type));
        Host->GetEditorSubsystem<DReflectionService>()->EditTarget = NewObject<DExcludedScriptGrandchild>(Host.Get(), "Excluded");
        Pass(R"PY(
import re
assert issubclass(durin.types.Durin.PythonFixtures.InheritedScriptType, durin.types.Durin.PythonFixtures.ScriptTypeBase)
assert issubclass(durin.types.Durin.PythonFixtures.InheritedScriptGrandchild, durin.types.Durin.PythonFixtures.InheritedScriptType)
assert durin.get_type('Durin::PythonFixtures::DInheritedScriptGrandchild') is durin.types.Durin.PythonFixtures.InheritedScriptGrandchild
for name in ('ExcludedScriptType', 'ExcludedScriptGrandchild'):
    assert not hasattr(durin.types.Durin.PythonFixtures, name)
    try:
        durin.get_type('Durin::PythonFixtures::D' + name)
    except AttributeError:
        pass
    else:
        raise AssertionError('Excluded class hierarchy was exposed')
service = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
assert not hasattr(service, 'InheritedHiddenDependency')
try:
    service.GetEditTarget()
except TypeError:
    pass
else:
    raise AssertionError('Excluded dynamic type was wrapped through Object')
stubs = durin.generate_stubs()
parent = re.search(r'InheritedScriptType: _TypeAlias = (_Native\d+)', stubs)[1]
child = re.search(r'InheritedScriptGrandchild: _TypeAlias = (_Native\d+)', stubs)[1]
assert f'class {child}({parent}):' in stubs
assert 'ExcludedScriptType' not in stubs and 'ExcludedScriptGrandchild' not in stubs
)PY");
    }

    TEST_F(PythonReflectionTests, ExplicitCameraAndEmptyActorTypesSpawnInAnEmptyLevelWithSharedHistory)
    {
        PrepareScene(EPythonEditPolicy::NonDocumentReadWrite, false);
        ASSERT_TRUE(EditLevel->GetActors().empty());
        EXPECT_FALSE(ACameraActor::StaticClass()->GetScriptExposure().Has(EScriptExposure::Type));
        ActorPass(R"PY(
assert actors.GetAllLevelActors(level) == []
assert durin.get_type('Durin::ACameraActor') is durin.types.Durin.CameraActor
assert durin.get_type('Durin::AActor') is durin.types.Durin.Actor
assert issubclass(durin.types.Durin.CameraActor, durin.types.Durin.Actor)
assert issubclass(durin.types.Durin.CameraComponent, durin.types.Durin.ActorComponent)
with durin.ScopedEditorTransaction('Create camera and empty Actor'):
    empty = actors.SpawnActor(level, durin.types.Durin.Actor, durin.types.Durin.Transform(), name='PythonEmpty')
    camera = actors.SpawnActor(level, durin.types.Durin.CameraActor, durin.types.Durin.Transform(), name='PythonCamera')
    assert type(empty) is durin.types.Durin.Actor and type(camera) is durin.types.Durin.CameraActor
    assert actors.GetActorInfo(level, empty).root_component is None
    root = actors.GetActorInfo(level, camera).root_component
    assert isinstance(root, durin.types.Durin.CameraComponent)
    transform = root.get_editor_property('relative_transform')
    position = transform.translation
    position.x = 125.0
    transform.translation = position
    root.set_editor_property('relative_transform', transform)
assert 'CameraActor: _TypeAlias = ' in durin.generate_stubs()
)PY");
        auto* Empty = EditLevel->FindActorByName("PythonEmpty"); ASSERT_NE(Empty, nullptr);
        auto* Camera = EditLevel->FindActorByName("PythonCamera"); ASSERT_NE(Camera, nullptr);
        EXPECT_DOUBLE_EQ(Camera->GetActorTransform().Translation.x, 125);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_TRUE(EditLevel->GetActors().empty());
        EXPECT_FALSE(ScenePackage->IsDirty());
        ASSERT_TRUE(Host->GetTransactor()->Redo());
        EXPECT_EQ(EditLevel->FindActorByName("PythonEmpty"), Empty);
        EXPECT_EQ(EditLevel->FindActorByName("PythonCamera"), Camera);
        EXPECT_DOUBLE_EQ(Camera->GetActorTransform().Translation.x, 125);
    }

    TEST_F(PythonReflectionTests, ActorLabelsAreIndependentTransactionalAndExposeExplicitNameModes)
    {
        PrepareScene(EPythonEditPolicy::NonDocumentReadWrite, false);
        ActorPass(R"PY(
with durin.ScopedEditorTransaction('Label and tags'):
    item = actors.SpawnActor(level, durin.types.Durin.Actor, durin.types.Durin.Transform(), name='Internal', label='Display')
    builtins.label_actor = item
    assert item.get_name() == 'Internal' and item.GetActorLabel() == 'Display'
    assert actors.GetActorInfo(level, item).label == 'Display'
    assert actors.SetActorLabel(level, item, 'Renamed display')
    item.set_editor_property('tags', [durin.Name('Example.Marker')])
assert item.get_name() == 'Internal'
assert actors.SpawnActor(level, durin.types.Durin.Actor, durin.types.Durin.Transform(), name='Internal', name_mode=durin.types.Durin.SpawnActorNameMode.REQUIRED_RETURN_NULL) is None
assert actors.RenameActor(level, item, 'Internal', name_mode=durin.types.Durin.SpawnActorNameMode.REQUIRED_RETURN_NULL)
assert actors.SetActorLabel(level, item, 'Renamed display', make_unique=False)
)PY");
        ASSERT_TRUE(Host->GetTransactor()->Undo());
        EXPECT_TRUE(EditLevel->GetActors().empty());
        ASSERT_TRUE(Host->GetTransactor()->Redo());
        ActorPass(R"PY(
item = builtins.label_actor
assert item.GetActorLabel() == 'Renamed display'
assert item.get_editor_property('tags') == [durin.Name('Example.Marker')]
assert actors.GetActorInfo(level, item).name == 'Internal'
assert actors.SetActorLabel(level, item, 'Renamed display')
)PY");
    }

    TEST_F(PythonReflectionTests, ActorQueriesReturnTypedDetachedSnapshotsAndRejectInvalidTargets)
    {
        ActorPass("assert level is None");
        PrepareActorScene();
        ActorPass(R"(
assert actors is durin.get_editor_subsystem(durin.get_type('Durin::DEditorActorSubsystem'))
assert not hasattr(durin.types.Durin, 'ActorScriptEditResult')
assert not hasattr(durin.types.Durin, 'ActorScriptHistoryResult')
assert not hasattr(durin.types.Durin, 'ActorEditStatus')
assert not hasattr(durin.types.Durin, 'ActorEditError')
items = actors.GetAllLevelActors(level)
assert [a.get_name() for a in items] == ['Editable', 'Seed']
rootless = actors.GetActorInfo(level, items[0])
assert rootless.root_component is None and rootless.parent is None
assert rootless.world_transform.translation.x == 0
info = actors.GetActorInfo(level, items[1])
assert info.actor is items[1] and info.actor_class is type(items[1])
assert info.root_component is not None and info.children == []
items.clear()
assert len(actors.GetAllLevelActors(level)) == 2
for operation in [lambda: actors.GetAllLevelActors(None), lambda: actors.GetActorInfo(level, None)]:
    try:
        operation()
    except durin.ScriptAccessError:
        pass
    else:
        raise AssertionError('query rejection became an empty snapshot')
try:
    info.name = 'Bypass'
except AttributeError:
    pass
else:
    raise AssertionError('query result is writable')
)");
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ActorNoOpsLeavePackageAndHistoryUnchanged)
    {
        PrepareActorScene();
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
assert actors.RenameActor(level, seed, 'Seed') is True
assert actors.DetachActors(level, [seed]) is False
)");
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
        EXPECT_FALSE(ScenePackage->IsDirty());
    }
    TEST_F(PythonReflectionTests, ActorSpawnRenameAndTransformShareHistoryAndIdentity)
    {
        PrepareActorScene();
        ActorPass(R"(
seed = actors.GetActorInfo(level, actors.GetAllLevelActors(level)[-1])
with durin.ScopedEditorTransaction('Create and move preview Actor'):
    created = actors.SpawnActor(level, seed.actor_class, seed.world_transform, name='PreviewActor')
    assert isinstance(created, durin.types.Durin.Actor)
    builtins.preview = created
    renamed = actors.RenameActor(level, builtins.preview, 'PreviewActorRenamed')
    assert renamed is True
    root = actors.GetActorInfo(level, builtins.preview).root_component
    relative = root.get_editor_property('relative_transform')
    translation = relative.translation
    translation.x += 100
    relative.translation = translation
    root.set_editor_property('relative_transform', relative)
    unchanged = actors.RenameActor(level, builtins.preview, 'PreviewActorRenamed')
    assert unchanged is True
)");
        auto* Preview = EditLevel->FindActorByName("PreviewActorRenamed"); ASSERT_NE(Preview, nullptr);
        EXPECT_DOUBLE_EQ(Preview->GetActorTransform().Translation.x, 100);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_FALSE(EditLevel->ContainsActor(Preview));
        EXPECT_EQ(Preview->GetFName(), FName("PreviewActor"));
        EXPECT_DOUBLE_EQ(Preview->GetActorTransform().Translation.x, 0);
        EXPECT_FALSE(ScenePackage->IsDirty());
        ActorPass(R"(
assert builtins.preview not in actors.GetAllLevelActors(level)
failed = actors.RenameActor(level, builtins.preview, 'Absent')
assert failed is False
)");
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(EditLevel->FindActorByName("PreviewActorRenamed"), Preview);
        EXPECT_DOUBLE_EQ(Preview->GetActorTransform().Translation.x, 100);
        ActorPass("assert actors.GetActorInfo(level, builtins.preview).actor is builtins.preview");
    }
    TEST_F(PythonReflectionTests, ActorDuplicateAttachDetachAndDeletePreserveNativeBatchSemantics)
    {
        PrepareActorScene();
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
with durin.ScopedEditorTransaction('Duplicate and attach preview'):
    copied = actors.DuplicateActors(level, [seed])
    assert isinstance(copied, list) and len(copied) == 1
    builtins.duplicate = copied[0]
    assert actors.AttachActors(level, [builtins.duplicate], seed) is True
    assert actors.GetActorInfo(level, seed).children == [builtins.duplicate]
    assert actors.AttachActors(level, [builtins.duplicate], seed) is False
    assert actors.AttachActors(level, [seed], builtins.duplicate) is False
    assert actors.DetachActors(level, [builtins.duplicate]) is True
    assert actors.DetachActors(level, [builtins.duplicate], durin.types.Durin.DetachmentTransformRule.KEEP_RELATIVE) is False
    failed = actors.DestroyActors(level, [seed, seed])
    assert failed is False
    assert seed in actors.GetAllLevelActors(level)
)");
        auto* Copy = EditLevel->FindActorByName("Seed_2"); ASSERT_NE(Copy, nullptr);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_FALSE(EditLevel->ContainsActor(Copy));
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_TRUE(EditLevel->ContainsActor(Copy));
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-2]
assert builtins.duplicate in actors.GetAllLevelActors(level)
deleted = actors.DestroyActors(level, [seed, builtins.duplicate])
assert deleted is True
assert len(actors.GetAllLevelActors(level)) == 1
assert builtins.duplicate.get_name() == 'Seed_2'
)");
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_TRUE(EditLevel->ContainsActor(Copy));
        ActorPass("assert actors.GetActorInfo(level, builtins.duplicate).actor is builtins.duplicate");
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_FALSE(EditLevel->ContainsActor(Copy));
    }
    TEST_F(PythonReflectionTests, ActorBatchesPreserveInputOrderAndLevelOrderedChildren)
    {
        PrepareActorScene();
        ASSERT_NE(EditLevel->SpawnActor<ACameraActor>("Second"), nullptr);
        ActorPass(R"(
seed, second = actors.GetAllLevelActors(level)[1:]
with durin.ScopedEditorTransaction('Actor batch'):
    copied = actors.DuplicateActors(level, [second, seed])
    assert copied
    builtins.batch = copied
    assert [a.get_name() for a in builtins.batch] == ['Second_2', 'Seed_2']
    attached = actors.AttachActors(level, list(reversed(builtins.batch)), seed, durin.types.Durin.AttachmentTransformRule.SNAP_TO_TARGET)
    assert attached is True
    assert actors.GetActorInfo(level, seed).children == builtins.batch
    assert all((actors.GetActorInfo(level, a).parent is seed for a in builtins.batch))
    assert actors.DetachActors(level, builtins.batch) is True
    failed = actors.DestroyActors(level, [seed, None])
    assert failed is False
    assert len(actors.GetAllLevelActors(level)) == 5
)");
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(EditLevel->GetActors().size(), 3u);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(EditLevel->GetActors().size(), 5u);
        ActorPass("assert all(a in actors.GetAllLevelActors(level) for a in builtins.batch)");
    }
    TEST_F(PythonReflectionTests, ActorEditsRequireDocumentAuthorityAndRespectReadOnlyPolicies)
    {
        PrepareActorScene(EPythonEditPolicy::DocumentManaged);
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
result = actors.RenameActor(level, seed, 'Denied')
assert result is False
)");
        Runtime.CloseSession(Session);
        const auto ReadOnly = Runtime.CreateSession(Tools->GetService(), EToolSource::External, {}, {.EditPolicy = EPythonEditPolicy::NonDocumentReadOnly});
        ASSERT_TRUE(ReadOnly); Session = *ReadOnly;
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
assert actors.DestroyActors(level, [seed]) is False
)");
        Runtime.CloseSession(Session);
        const auto Writable = Runtime.CreateSession(Tools->GetService(), EToolSource::External, {}, {.EditPolicy = EPythonEditPolicy::NonDocumentReadWrite});
        ASSERT_TRUE(Writable); Session = *Writable;
        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [] { return EEditorScriptTargetAccess::ReadOnly; });
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
assert not actors.DuplicateActors(level, [seed])
assert len(actors.GetAllLevelActors(level)) == 2
)");
        EXPECT_EQ(EditLevel->FindActorByName("Seed")->GetFName(), FName("Seed"));
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ActorCallsRejectForeignLevelsInvalidClassesAndHierarchyBatches)
    {
        PrepareActorScene();
        auto* Foreign = NewObject<DLevel>(ScenePackage.Get(), "Foreign");
        Host->GetEditorSubsystem<DReflectionService>()->EditTarget = Foreign;
        ActorPass(R"(
seed = actors.GetAllLevelActors(level)[-1]
info = actors.GetActorInfo(level, seed)
foreign = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
assert actors.SpawnActor(foreign, info.actor_class, durin.types.Durin.Transform()) is None
assert actors.SpawnActor(None, info.actor_class, durin.types.Durin.Transform()) is None
assert actors.SpawnActor(level, durin.types.Durin.EditorActorSubsystem, durin.types.Durin.Transform()) is None
assert actors.DestroyActors(level, []) is False
assert not actors.DuplicateActors(level, [seed, seed])
assert actors.AttachActors(level, [seed, seed], seed) is False
assert actors.AttachActors(level, [seed], seed) is False
for operation in [lambda: actors.GetAllLevelActors(foreign), lambda: actors.GetActorInfo(foreign, seed)]:
    try:
        operation()
    except durin.ScriptAccessError:
        pass
    else:
        raise AssertionError('foreign query admitted')
for operation in [lambda: actors.SpawnActor(level, info.actor_class), lambda: actors.DetachActors(level, [seed], 0)]:
    try:
        operation()
    except TypeError:
        pass
    else:
        raise AssertionError('invalid signature admitted')
assert actors.GetAllLevelActors(level)[-1] is seed
)");
        EXPECT_TRUE(Foreign->GetActors().empty()); EXPECT_EQ(EditLevel->GetActors().size(), 2u);
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ActorQueriesUseEditorLevelDuringPieButEditsAreDenied)
    {
        PrepareActorScene();
        FEditorEngineTestAccess::SetState(*Host, EPlayState::Playing);
        const auto Result = Run(R"(
actors = durin.get_editor_subsystem(durin.types.Durin.EditorActorSubsystem)
level = actors.GetEditorLevel()
seed = actors.GetAllLevelActors(level)[-1]
assert actors.GetActorInfo(level, seed).name == 'Seed'
assert actors.RenameActor(level, seed, 'DuringPIE') is False
)");
        FEditorEngineTestAccess::SetState(*Host, EPlayState::Stopped);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ActorConstructionRechecksPolicyAndHandlesRetiredCandidates)
    {
        PrepareActorScene(); bool Writable = true;
        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [&] {
            return Writable ? EEditorScriptTargetAccess::Writable : EEditorScriptTargetAccess::ReadOnly;
        });
        AReflectionLifecycleActor::ConstructCallback = [&](AReflectionLifecycleActor&) { Writable = false; };
        ActorPass(R"(
result = actors.SpawnActor(level, durin.types.Durin.PythonFixtures.ReflectionLifecycleActor, durin.types.Durin.Transform(), 'DeniedConstruction')
assert result is None
assert len(actors.GetAllLevelActors(level)) == 2
)");
        Writable = true;
        AReflectionLifecycleActor::ConstructCallback = [](AReflectionLifecycleActor& Actor) { MarkObjectHierarchyAsGarbage(&Actor); };
        ActorPass(R"(
result = actors.SpawnActor(level, durin.types.Durin.PythonFixtures.ReflectionLifecycleActor, durin.types.Durin.Transform(), 'RetiredConstruction')
assert result is None
assert len(actors.GetAllLevelActors(level)) == 2
)");
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ActorPreparationCannotPublishAfterHostClosure)
    {
        PrepareActorScene();
        AReflectionLifecycleActor::ConstructCallback = [&](AReflectionLifecycleActor&) { Host->RequestShutdown(); };
        const auto Result = Run(R"(
actors = durin.get_editor_subsystem(durin.types.Durin.EditorActorSubsystem)
actors.SpawnActor(actors.GetEditorLevel(), durin.types.Durin.PythonFixtures.ReflectionLifecycleActor, durin.types.Durin.Transform(), 'ClosedHost')
)");
        EXPECT_NE(Result.State, EPythonJobState::Succeeded);
        EXPECT_EQ(EditLevel->FindActorByName("ClosedHost"), nullptr);
        EXPECT_EQ(EditLevel->GetActors().size(), 2u);
    }
    TEST_F(PythonReflectionTests, ActorDuplicationRechecksPolicyAfterPostLoad)
    {
        PrepareActorScene();
        auto* Source = EditLevel->SpawnActor<AReflectionLifecycleActor>("Lifecycle"); ASSERT_NE(Source, nullptr);
        bool Writable = true;
        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [&] {
            return Writable ? EEditorScriptTargetAccess::Writable : EEditorScriptTargetAccess::ReadOnly;
        });
        AReflectionLifecycleActor::LoadCallback = [&](AReflectionLifecycleActor&) { Writable = false; };
        ActorPass(R"(
source = actors.GetAllLevelActors(level)[-1]
result = actors.DuplicateActors(level, [source])
assert result == []
assert len(actors.GetAllLevelActors(level)) == 3
)");
        EXPECT_TRUE(EditLevel->ContainsActor(Source)); EXPECT_FALSE(Host->GetTransactor()->CanUndo());
        Writable = true;
        AReflectionLifecycleActor::LoadCallback = [](AReflectionLifecycleActor& Actor) { MarkObjectHierarchyAsGarbage(&Actor); };
        ActorPass(R"(
source = actors.GetAllLevelActors(level)[-1]
assert not actors.DuplicateActors(level, [source])
assert len(actors.GetAllLevelActors(level)) == 3
builtins.stale_actor = source
)");
        MarkObjectHierarchyAsGarbage(Source);
        ActorPass(R"(
try:
    actors.RenameActor(level, builtins.stale_actor, 'Stale')
except ReferenceError:
    pass
else:
    raise AssertionError('stale Actor argument reached native editing')
)");
    }
    TEST_F(PythonReflectionTests, EditorPropertyWritesNormalizeNotifyAndRecordHistory)
    {
        PrepareScene();
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
assert a.value == 1
assert a.set_editor_property('value', 200) is None
assert a.value == 100 and a.get_editor_property('value') == 100
)");
        EXPECT_EQ(EditActor->Value, 100);
        EXPECT_EQ(EditActor->Validations, 1);
        EXPECT_EQ(EditActor->Changes, (std::vector{EPropertyChangePhase::Interactive, EPropertyChangePhase::Committed}));
        EXPECT_TRUE(ScenePackage->IsDirty());
        ASSERT_TRUE(Host->GetTransactor()->CanUndo());
        EXPECT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(EditActor->Value, 1);
        EXPECT_FALSE(ScenePackage->IsDirty());
        EXPECT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(EditActor->Value, 100);
        EXPECT_TRUE(ScenePackage->IsDirty());
    }
    TEST_F(PythonReflectionTests, PropertySnapshotsAndNestedValuesShareOneTransaction)
    {
        PrepareScene();
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
with durin.ScopedEditorTransaction('Python compound properties'):
    a.value = 9
    value = a.get_editor_property('data')
    value.text = 'edited'
    value.fixed = [7, 8, 9]
    pose = value.pose
    translation = pose.translation
    translation.x = 23
    pose.translation = translation
    value.pose = pose
    a.set_editor_property('data', value)
    a.set_editor_property('numbers', [1, 2, 3])
    a.set_editor_property('mapping', {'key': 4})
numbers = a.get_editor_property('numbers')
numbers.append(99)
assert a.get_editor_property('numbers') == [1, 2, 3]
mapping = a.get_editor_property('mapping')
mapping[durin.Name('key')] = 99
assert a.get_editor_property('mapping')[durin.Name('key')] == 4
assert a.get_editor_property('data').pose.translation.x == 23
)");
        EXPECT_EQ(Host->GetTransactor()->GetQueueLength(), 1);
        ASSERT_TRUE(Host->GetTransactor()->Undo());
        EXPECT_EQ(EditActor->Value, 1); EXPECT_EQ(EditActor->Data.Text, "initial"); EXPECT_TRUE(EditActor->Numbers.empty());
        EXPECT_TRUE(EditActor->Mapping.empty()); EXPECT_DOUBLE_EQ(EditActor->Data.Pose.Translation.x, 0);
        ASSERT_TRUE(Host->GetTransactor()->Redo());
        EXPECT_EQ(EditActor->Data.Fixed[2], 9); EXPECT_EQ(EditActor->Data.Text, "edited");
        EXPECT_DOUBLE_EQ(EditActor->Data.Pose.Translation.x, 23); EXPECT_EQ(EditActor->Numbers.size(), 3);
    }
    TEST_F(PythonReflectionTests, RejectedPropertyPreservesEarlierEditsAndOuterHistory)
    {
        PrepareScene();
        const auto Result = Run(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
with durin.ScopedEditorTransaction('Keep completed edits'):
    a.value = 10
    a.value = -1
)");
        EXPECT_EQ(Result.State, EPythonJobState::Failed); EXPECT_NE(Result.Error.find("ValueError"), std::string::npos);
        EXPECT_EQ(EditActor->Value, 10); EXPECT_EQ(Host->GetTransactor()->GetQueueLength(), 1);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(EditActor->Value, 1);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(EditActor->Value, 10);
    }
    TEST_F(PythonReflectionTests, PropertyExposureAndUnsupportedTargetsCannotBypassEditing)
    {
        PrepareScene();
        Pass(R"(
s = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
a = s.GetEditTarget()
for operation in (lambda: setattr(a, 'helper_only', 8),
                  lambda: a.set_editor_property('locked', 8),
                  lambda: a.set_editor_property('unrecorded', 8),
                  lambda: a.get_editor_property('secret'),
                  lambda: a.set_editor_property('secret', 8),
                  lambda: a.set_editor_property('fixed', [4, 5, 6])):
    try:
        operation()
    except AttributeError:
        pass
    else:
        raise AssertionError('restricted property accessed')
for operation in (lambda: setattr(s, 'service_value', 9), lambda: s.set_editor_property('service_value', 9)):
    try:
        operation()
    except durin.ScriptAccessError:
        pass
    else:
        raise AssertionError('unowned subsystem write admitted')
assert a.get_editor_property('fixed') == [1, 2, 3]
a.set_editor_property('helper_only', 8)
assert a.helper_only == 8
a.enabled = True
assert a.get_editor_property('enabled') is True
before = a.get_editor_property('numbers')
try:
    a.set_editor_property('numbers', [1, 'bad'])
except TypeError:
    pass
else:
    raise AssertionError('partial container input admitted')
assert a.get_editor_property('numbers') == before
)");
        EXPECT_EQ(EditActor->Locked, 3); EXPECT_EQ(EditActor->Unrecorded, 4); EXPECT_EQ(EditActor->Secret, 5);
        EXPECT_EQ(Host->GetEditorSubsystem<DReflectionService>()->ServiceValue, 7);
    }
    TEST_F(PythonReflectionTests, DocumentPolicyIsRecheckedAfterNativeValidation)
    {
        PrepareScene(); ASSERT_FALSE(Host->GetTransactor()->CanUndo()) << "Fixture already has history"; bool Writable = true;
        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [&] {
            return Writable ? EEditorScriptTargetAccess::Writable : EEditorScriptTargetAccess::ReadOnly;
        });
        EditActor->BeforePublication = [&] { Writable = false; };
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
try:
    a.value = 19
except durin.ScriptAccessError:
    pass
else:
    raise AssertionError('changed document policy bypassed')
assert a.value == 1
)");
        EXPECT_EQ(EditActor->Validations, 1); EXPECT_EQ(EditActor->Value, 1);
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ReadOnlyAndDocumentManagedSessionsRejectWrites)
    {
        PrepareScene(EPythonEditPolicy::DocumentManaged);
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
try:
    a.set_editor_property('value', 5)
except durin.ScriptAccessError:
    pass
else:
    raise AssertionError('missing document authority admitted')
assert a.value == 1
)");
        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [] { return EEditorScriptTargetAccess::ReadOnly; });
        const auto RW = Runtime.CreateSession(Tools->GetService(), EToolSource::External, {}, {.EditPolicy = EPythonEditPolicy::NonDocumentReadWrite});
        ASSERT_TRUE(RW);
        const auto Result = Run(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
try:
    a.value = 5
except durin.ScriptAccessError:
    pass
else:
    raise AssertionError('headless option overrode document read-only')
)", *RW);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        EXPECT_EQ(EditActor->Validations, 0); EXPECT_FALSE(Host->GetTransactor()->CanUndo());
    }
    TEST_F(PythonReflectionTests, ForeignLevelAndStalePropertyTargetsAreRejected)
    {
        PrepareScene();
        auto* Foreign = NewObject<DLevel>(ScenePackage.Get(), "ForeignLevel");
        auto* ForeignActor = Foreign->SpawnActor<AReflectionActor>("ForeignActor"); ASSERT_NE(ForeignActor, nullptr);
        Host->GetEditorSubsystem<DReflectionService>()->EditTarget = ForeignActor;
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
try:
    a.value = 9
except durin.ScriptAccessError:
    pass
else:
    raise AssertionError('foreign Level write admitted')
builtins.stale_setter = a.set_editor_property
)");
        EXPECT_EQ(ForeignActor->Value, 1); EXPECT_EQ(ForeignActor->Validations, 0);
        MarkAsGarbage(ForeignActor);
        Pass(R"(
try:
    builtins.stale_setter('value', 9)
except ReferenceError:
    pass
else:
    raise AssertionError('stale setter admitted')
)");
    }
    TEST_F(PythonReflectionTests, ComponentTransformWritesUseNativeNotificationsAndHistory)
    {
        PrepareScene();
        auto* Component = Cast<DSceneComponent>(EditActor->AddInstanceComponent(DSceneComponent::StaticClass(), "Root"));
        ASSERT_NE(Component, nullptr); ASSERT_TRUE(EditActor->SetRootComponent(Component));
        Host->GetEditorSubsystem<DReflectionService>()->EditTarget = Component;
        Pass(R"(
component = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget()
pose = component.get_editor_property('relative_transform')
translation = pose.translation
translation.x = 42
pose.translation = translation
component.set_editor_property('relative_transform', pose)
)");
        EXPECT_DOUBLE_EQ(Component->GetRelativeTransform().Translation.x, 42);
        EXPECT_DOUBLE_EQ(Component->GetWorldTransform().Translation.x, 42);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_DOUBLE_EQ(Component->GetWorldTransform().Translation.x, 0);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_DOUBLE_EQ(Component->GetWorldTransform().Translation.x, 42);
    }
    TEST_F(PythonReflectionTests, FindsNonRootComponentAndUndoesDiscoveredPropertyEdit)
    {
        PrepareScene();
        auto* Root = Cast<DSceneComponent>(EditActor->AddInstanceComponent(DSceneComponent::StaticClass(), "Root"));
        ASSERT_NE(Root, nullptr);
        ASSERT_TRUE(EditActor->SetRootComponent(Root));
        auto* Camera = Cast<DCameraComponent>(EditActor->AddInstanceComponent(DCameraComponent::StaticClass(), "Lens"));
        ASSERT_NE(Camera, nullptr);
        ASSERT_NE(Camera, EditActor->GetRootComponent());
        const auto OriginalX = Camera->GetRelativeTransform().Translation.x;
        ASSERT_FALSE(Host->GetTransactor()->CanUndo());

        ActorPass(R"PY(
types = durin.types.Durin
actor = actors.GetAllLevelActors(level)[0]
root = actors.GetActorInfo(level, actor).root_component
camera, = actors.GetComponents(level, actor, component_class=types.CameraComponent)
assert type(camera) is types.CameraComponent and camera is not root
assert actors.GetComponents(level, actor) == [root, camera]
assert actors.GetComponents(level, actor, types.SceneComponent) == [root, camera]
assert actors.GetComponents(level, actor, types.ActorComponent) == [root, camera]
for bad_level, bad_type in ((None, types.CameraComponent), (level, types.Actor)):
    try:
        actors.GetComponents(bad_level, actor, bad_type)
    except durin.ScriptAccessError:
        pass
    else:
        raise AssertionError('invalid component query succeeded')
properties = camera.list_editor_properties()
assert properties['relative_transform'] == {'type': 'Durin::FTransform', 'writable': True}
assert 'world_transform' not in properties
pose = camera.get_editor_property('relative_transform')
translation = pose.translation
translation.x = 42
pose.translation = translation
with durin.ScopedEditorTransaction('Move camera component'):
    camera.set_editor_property('relative_transform', pose)
assert camera.get_editor_property('relative_transform').translation.x == 42
)PY");
        EXPECT_DOUBLE_EQ(Camera->GetRelativeTransform().Translation.x, 42);
        ASSERT_EQ(Host->GetTransactor()->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo());
        EXPECT_DOUBLE_EQ(Camera->GetRelativeTransform().Translation.x, OriginalX);
        EXPECT_FALSE(Host->GetTransactor()->CanUndo());

        FEditorScriptTargetPolicyRegistration Policy(*Host, *EditLevel, [] { return EEditorScriptTargetAccess::ReadOnly; });
        ActorPass(R"PY(
actor = actors.GetAllLevelActors(level)[0]
camera, = actors.GetComponents(level, actor, durin.types.Durin.CameraComponent)
assert camera.list_editor_properties()['relative_transform']['writable'] is False
camera.get_editor_property('relative_transform')
)PY");
    }
    TEST_F(PythonReflectionTests, ClosedHostRejectsCachedPropertySetter)
    {
        PrepareScene();
        Pass("builtins.closed_setter = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService).GetEditTarget().set_editor_property");
        Host->PreExit();
        TStrongObjectPtr<DEditorEngine> Other(NewObject<DEditorEngine>(nullptr, {}));
        ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Other));
        auto* OtherTools = Other->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(OtherTools, nullptr);
        const auto Created = Runtime.CreateSession(OtherTools->GetService()); ASSERT_TRUE(Created);
        const auto Result = Run(R"(
try:
    builtins.closed_setter('value', 9)
except ReferenceError:
    pass
else:
    raise AssertionError('closed Host property setter admitted')
)", *Created, OtherTools);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        EXPECT_EQ(EditActor->Value, 1); EXPECT_EQ(EditActor->Validations, 0);
        Runtime.CloseSession(*Created); Other->PreExit();
        MarkObjectHierarchyAsGarbage(Other.Get()); Other.Reset();
    }
    TEST_F(PythonReflectionTests, DiscoveryAndStubsDescribePublishedRuntimeMembers)
    {
        Pass(R"PY(
import ast
import pydoc
assert 'Durin' in dir(durin.types)
assert 'EditorActorSubsystem' in dir(durin.types.Durin)
assert 'EditorActorSubsystem' not in dir(durin)
assert 'SpawnActor' in dir(durin.types.Durin.EditorActorSubsystem)
assert 'spawn_actor' not in dir(durin.types.Durin.EditorActorSubsystem)
assert 'ScriptSpawnActor' not in dir(durin.types.Durin.EditorActorSubsystem)
assert 'EditorActorSubsystem' in durin.types.Durin.EditorActorSubsystem.__doc__
assert 'SpawnActor(self' in durin.types.Durin.EditorActorSubsystem.SpawnActor.__doc__
assert 'SpawnActor(self' in pydoc.render_doc(durin.types.Durin.EditorActorSubsystem.SpawnActor, renderer=pydoc.plaintext)
service = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
assert 'Add(self, /, value: int, amount: int = ...)' in service.Add.__doc__
assert not hasattr(service, 'add')
assert '-> int | None' in service.TryValue.__doc__
secondary = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.SecondaryService)
assert 'JoinValues(self' in secondary.JoinValues.__doc__
assert {'join_values', 'Join'}.isdisjoint(dir(secondary))
source = durin.generate_stubs()
assert source == durin.generate_stubs()
tree = ast.parse(source)
compile(tree, '<durin-stubs>', 'exec')
classes = {node.name: node for node in tree.body if isinstance(node, ast.ClassDef)}
native = next(node for node in classes['types'].body if isinstance(node, ast.ClassDef) and node.name == 'Durin')
fixtures = next(node for node in native.body if isinstance(node, ast.ClassDef) and node.name == 'PythonFixtures')
def aliases(scope):
    return {node.target.id: node.value.id for node in scope.body if isinstance(node, ast.AnnAssign)}
assert 'SharedAlias' not in aliases(fixtures)
service = classes[aliases(fixtures)['ReflectionService']]
methods = {node.name: node for node in service.body if isinstance(node, ast.FunctionDef)}
assert ast.unparse(methods['Multi'].returns) == 'tuple[int, str, int]'
assert ast.unparse(methods['TryValue'].returns) == 'int | None'
assert not methods['NativeOnly'].args.defaults
assert len(methods['Add'].args.defaults) == 1
assert 'Unsupported' not in methods
secondary = classes[aliases(fixtures)['SecondaryService']]
methods = {node.name for node in secondary.body if isinstance(node, ast.FunctionDef)}
assert 'JoinValues' in methods and {'join_values', 'Join'}.isdisjoint(methods)
actor = classes[aliases(native)['EditorActorSubsystem']]
methods = {node.name: node for node in actor.body if isinstance(node, ast.FunctionDef)}
actor_class = aliases(native)['Actor']
assert ast.unparse(methods['SpawnActor'].returns) == f'{actor_class} | None'
assert ast.unparse(methods['DuplicateActors'].returns) == f'list[{actor_class} | None]'
assert ast.unparse(methods['DestroyActors'].returns) == 'bool'
first = next(node for node in fixtures.body if isinstance(node, ast.ClassDef) and node.name == 'First')
second = next(node for node in fixtures.body if isinstance(node, ast.ClassDef) and node.name == 'Second')
assert classes[aliases(first)['SharedType']] is not classes[aliases(second)['SharedType']]
assert 'FActorEditResult' not in source and 'ActorScriptEditResult' not in source
builtins.reflection_stub_source = source
)PY");
        const auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created);
        const auto Result = Run("assert durin.generate_stubs() == builtins.reflection_stub_source", *Created);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
    }

    TEST_F(PythonReflectionTests, RuntimeTypesPreserveInheritanceAndFactoryIdentity)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
b = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.SecondaryService)
assert a is durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
assert type(a) is durin.types.Durin.PythonFixtures.ReflectionService and a.get_class() is type(a)
assert isinstance(a, durin.types.Durin.PythonFixtures.ReflectionBase) and isinstance(a, durin.types.Durin.EditorSubsystem)
assert issubclass(durin.types.Durin.EditorSubsystem, durin.types.Durin.DynamicSubsystem)
assert issubclass(durin.types.Durin.DynamicSubsystem, durin.types.Durin.Subsystem)
assert a != b and a == a and isinstance(a.get_name(), str)
assert durin.get_type('Durin::PythonFixtures::DReflectionService') is type(a)
assert isinstance(durin.get_type('Durin::PythonFixtures::FSignatureDependency'), type)
assert isinstance(durin.types.Durin.PythonFixtures.ReflectionMode, type)
assert not hasattr(durin.types.Durin.PythonFixtures, 'SharedAlias')
assert durin.get_type('Durin::PythonFixtures::DFirstAlias') is not durin.get_type('Durin::PythonFixtures::DSecondAlias')
)");
    }
    TEST_F(PythonReflectionTests, SameNameTypesCoexistInDifferentNamespaces)
    {
        Pass(R"PY(
fixtures = durin.types.Durin.PythonFixtures
assert {'First', 'Second'} <= set(dir(fixtures))
assert 'SharedType' in dir(fixtures.First) and 'SharedType' in dir(fixtures.Second)
first = fixtures.First.SharedType
second = fixtures.Second.SharedType
assert first is not second
assert first is durin.get_type('Durin::PythonFixtures::First::DSharedType')
assert second is durin.get_type('Durin::PythonFixtures::Second::DSharedType')
assert first.__module__ == 'durin.types.Durin.PythonFixtures.First'
assert second.__module__ == 'durin.types.Durin.PythonFixtures.Second'
)PY");
    }
    TEST_F(PythonReflectionTests, NativeConstructionSubclassingAndHiddenTypesAreRejected)
    {
        Pass(R"(
for operation in (lambda: durin.types.Durin.PythonFixtures.ReflectionService(), lambda: type('Derived', (durin.types.Durin.PythonFixtures.ReflectionService,), {}),
                  lambda: type('DerivedValue', (durin.types.Durin.PythonFixtures.SignatureDependency,), {}),
                  lambda: type('DerivedEnum', (durin.types.Durin.PythonFixtures.ReflectionMode,), {}),
                  lambda: durin.get_editor_subsystem(int), lambda: setattr(durin.types.Durin.PythonFixtures.ReflectionService, 'extra', 1)):
    try:
        operation()
    except TypeError:
        pass
    else:
        raise AssertionError('native construction/type mutation admitted')
try:
    durin.get_type('Durin::PythonFixtures::DHiddenType')
except AttributeError:
    pass
else:
    raise AssertionError('excluded type exposed')
)");
    }
    TEST_F(PythonReflectionTests, GenericMethodsBindKeywordsDefaultsAndIndependentServices)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
b = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.SecondaryService)
assert a.Add(2) == 7
assert a.Add(amount=8, value=3) == 11
assert a.NativeOnly(19) == 19
assert b.JoinValues(['a', 'b']) == 'ab!'
assert b.JoinValues(('a', 'b'), suffix='?') == 'ab?'
assert a.Describe().value == 7
assert a.EchoObject(a) is a
assert a.EchoObject(None) is None
assert a.EchoClass(durin.types.Durin.PythonFixtures.ReflectionService) is durin.types.Durin.PythonFixtures.ReflectionService
assert a.EchoClass(None) is None
assert a.EchoMode(durin.types.Durin.PythonFixtures.ReflectionMode.READY) == durin.types.Durin.PythonFixtures.ReflectionMode.READY
assert not hasattr(a, 'HiddenDependency') and not hasattr(a, 'Unsupported')
assert a.Nothing() is None
assert a.calls == 9
)");
    }
    TEST_F(PythonReflectionTests, InheritanceUsesEffectiveOverrideAndMasksUnexposedMethods)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
assert a.VirtualValue(3) == 13
base = durin.types.Durin.PythonFixtures.ReflectionBase.VirtualValue.__get__(a, type(a))
assert base(4) == 14
assert durin.types.Durin.PythonFixtures.ReflectionBase.VirtualValue(a, 5) == 15
assert not hasattr(a, 'MaskedValue')
try:
    durin.types.Durin.PythonFixtures.ReflectionBase.MaskedValue.__get__(a, type(a))()
except AttributeError:
    pass
else:
    raise AssertionError('unexposed override called')
)");
    }
    TEST_F(PythonReflectionTests, InvalidArgumentsNeverEnterNativeMethod)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
invalid = [
    (TypeError, lambda: a.Add()),
    (TypeError, lambda: a.Add(1, value=2)),
    (TypeError, lambda: a.Add(1, unknown=2)),
    (TypeError, lambda: a.Add(1, 2, 3)),
    (TypeError, lambda: a.NativeOnly()),
    (TypeError, lambda: a.Add(True)),
    (TypeError, lambda: a.Add('1')),
    (OverflowError, lambda: a.Add(2**40)),
    (OverflowError, lambda: a.EchoSigned(128)),
    (OverflowError, lambda: a.EchoSigned(-129)),
    (OverflowError, lambda: a.EchoUnsigned(-1)),
    (OverflowError, lambda: a.EchoUnsigned(2**64)),
    (OverflowError, lambda: a.EchoFloat(1e100)),
    (ValueError, lambda: a.EchoFloat(float('nan'))),
    (ValueError, lambda: a.EchoFloat(float('inf'))),
    (TypeError, lambda: a.EchoFloat(True)),
    (TypeError, lambda: a.EchoMode(1)),
    (TypeError, lambda: a.EchoMode(durin.types.Durin.PythonFixtures.OtherMode.READY)),
    (TypeError, lambda: a.EchoObject(durin.types.Durin.PythonFixtures.ReflectionService)),
    (TypeError, lambda: a.EchoClass(a)),
    (TypeError, lambda: a.EchoValue({})),
    (TypeError, lambda: a.EchoValue(a.Describe())),
    (ValueError, lambda: a.EchoGuid('invalid')),
    (ValueError, lambda: a.EchoName('bad\x00name')),
]
for expected, operation in invalid:
    before = a.calls
    try:
        operation()
    except expected:
        pass
    else:
        raise AssertionError('invalid input accepted')
    assert a.calls == before
assert a.EchoSigned(-128) == -128
assert a.EchoUnsigned(2**64 - 1) == 2**64 - 1
)");
    }
    TEST_F(PythonReflectionTests, StructsAndMathAreTypedDetachedCopies)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
v = durin.types.Durin.PythonFixtures.ReflectionValue(text='hello', numbers=[1, 2], target=a)
p = v.pose
t = p.translation
t.x = 17.5
assert v.pose.translation.x == 0
p.translation = t
v.pose = p
copy = a.EchoValue(v)
assert type(copy) is durin.types.Durin.PythonFixtures.ReflectionValue
assert copy.target is a and copy.text == 'hello'
assert copy.pose.translation.x == 17.5
numbers = copy.numbers
numbers.append(3)
assert copy.numbers == [1, 2]
copy.numbers = numbers
assert copy.numbers == [1, 2, 3] and v.numbers == [1, 2]
assert v.fixed == [1, 2, 3]
v.fixed = (4, 5, 6)
assert a.EchoValue(v).fixed == [4, 5, 6]
for operation in (lambda: setattr(a, 'calls', 4), lambda: setattr(a.Describe(), 'value', 2),
                  lambda: durin.types.Durin.PythonFixtures.SignatureDependency(), lambda: durin.types.Durin.PythonFixtures.ReflectionValue(1),
                  lambda: durin.types.Durin.PythonFixtures.ReflectionValue(unknown=1)):
    try:
        operation()
    except (AttributeError, TypeError):
        pass
    else:
        raise AssertionError('restricted write/construction admitted')
try:
    v.numbers = [1, 'invalid']
except TypeError:
    pass
else:
    raise AssertionError('invalid detached edit accepted')
assert v.numbers == [1, 2]
for invalid in ([1, 2], [7, 'invalid', 9]):
    try:
        v.fixed = invalid
    except TypeError:
        pass
    else:
        raise AssertionError('invalid fixed array accepted')
assert v.fixed == [4, 5, 6]
)");
    }
    TEST_F(PythonReflectionTests, ContainerBudgetsCyclesAndKeyCollisionsRejectBeforeDispatch)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
key = durin.Name('item')
assert a.EchoMap({'item': [1, 2]}) == {key: [1, 2]}
assert a.EchoMap({key: (3, 4)}) == {key: [3, 4]}
cycle = []
cycle.append(cycle)
for operation in (lambda: a.EchoMap({'x': cycle}),
                  lambda: a.EchoMap({'item': [], key: []}),
                  lambda: a.EchoMap({'x': [0] * 65537}),
                  lambda: a.EchoValue(durin.types.Durin.PythonFixtures.ReflectionValue(text='x' * (16 * 1024 * 1024)))):
    before = a.calls
    try:
        operation()
    except (TypeError, ValueError):
        pass
    else:
        raise AssertionError('invalid container/budget accepted')
    assert a.calls == before
)");
    }
    TEST_F(PythonReflectionTests, ReturnAndOutPackingAndNativeErrors)
    {
        const auto LiveBefore = FReflectionValue::Live;
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
assert a.Multi('text', 2) == (42, 'text!', 3)
assert a.TryValue(True, 0) == 9
assert a.TryValue(False, 0) is None
for operation, expected in ((a.Reject, durin.ScriptAccessError),
                            (lambda: a.Throw(durin.types.Durin.PythonFixtures.ReflectionValue(text='temporary')), RuntimeError)):
    try:
        operation()
    except expected:
        pass
    else:
        raise AssertionError('native error lost')
assert a.Add(1) == 6
before = a.calls
try:
    a.OversizedOutput()
except ValueError:
    pass
else:
    raise AssertionError('output budget bypassed')
assert a.calls == before + 1
)");
        EXPECT_EQ(FReflectionValue::Live, LiveBefore);
    }
    TEST_F(PythonReflectionTests, NamesAndGuidsHaveTypedValueIdentity)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
name = durin.Name('fixture')
assert str(name) == 'fixture' and a.EchoName(name) == name
assert a.EchoName('fixture') == name
assert name == durin.Name('FIXTURE') and hash(name) == hash(durin.Name('FIXTURE'))
guid = durin.Guid('00112233-4455-6677-8899-aabbccddeeff')
assert a.EchoGuid(str(guid)) == guid
assert a.EchoGuid(guid) == guid
assert len({guid, a.EchoGuid(guid)}) == 1
assert durin.types.Durin.PythonFixtures.ReflectionMode.IDLE != durin.types.Durin.PythonFixtures.OtherMode.IDLE
)");
    }
    TEST_F(PythonReflectionTests, ReflectedCachedMethodsAndStructRootsRetireWithHost)
    {
        Pass(R"(
builtins.proxy = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
builtins.reflected = builtins.proxy.Add
builtins.value = durin.types.Durin.PythonFixtures.ReflectionValue(target=builtins.proxy)
)");
        CollectGarbage(Durin::EObjectFlags::Standalone);
        Pass("assert builtins.reflected(1) == 6\nassert builtins.value.target is builtins.proxy");
        ASSERT_NO_THROW(Host->RemoveEditorSubsystem(DReflectionService::StaticClass())); First.reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
        Pass(R"(
for operation in (lambda: builtins.reflected(1), lambda: builtins.value.target):
    try:
        operation()
    except ReferenceError:
        pass
    else:
        raise AssertionError('retired reference accessed')
)");
    }
    TEST_F(PythonReflectionTests, SameHostJobsAndSessionsReuseRetainedObjects)
    {
        Save(); CollectGarbage(Durin::EObjectFlags::Standalone);
        const auto Other = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Other);
        auto Result = Run("assert builtins.proxy is durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)\nassert builtins.cached() == builtins.proxy.get_name()", *Other);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        Runtime.CloseSession(Session);
        Result = Run("assert builtins.cached() == builtins.proxy.get_name()", *Other);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
    }
    TEST_F(PythonReflectionTests, RetiredEditorCachedMethodsRejectAfterSequentialReplacement)
    {
        Save(); Host->PreExit();
        TStrongObjectPtr<DEditorEngine> Other(NewObject<DEditorEngine>(nullptr, {})); ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Other));
        auto* OtherTools = Other->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(OtherTools, nullptr);
        auto Created = Runtime.CreateSession(OtherTools->GetService()); ASSERT_TRUE(Created);
        const auto Result = Run(R"(
assert builtins.proxy != durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('retired editor cached method admitted')
)", *Created, OtherTools);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        Runtime.CloseSession(*Created); Other->PreExit(); MarkObjectHierarchyAsGarbage(Other.Get()); Other.Reset();
        EXPECT_FALSE(Runtime.RunString(Session, "pass"));
    }
    TEST_F(PythonReflectionTests, RetiredServiceAccessPurgesProxyRootsAndCannotReviveCachedBoundMethods)
    {
        Save(); TWeakObjectPtr<DReflectionService> Old(Host->GetEditorSubsystem<DReflectionService>());
        ASSERT_NO_THROW(Host->RemoveEditorSubsystem(DReflectionService::StaticClass())); First.reset(); CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_TRUE(Old.IsValid());
        First = Register(DReflectionService::StaticClass()); ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
        Pass(R"(
assert hash(builtins.proxy) == builtins.identity
assert builtins.proxy != durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('retired proxy revived')
)");
        CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_FALSE(Old.IsValid());
    }

    TEST_F(PythonReflectionTests, DetachedValuesRejectAndReleaseRetiredServiceReferences)
    {
        Pass("builtins.value = durin.types.Durin.PythonFixtures.ReflectionValue(target=durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService))");
        TWeakObjectPtr<DReflectionService> Old(Host->GetEditorSubsystem<DReflectionService>());
        Host->RemoveEditorSubsystem(DReflectionService::StaticClass()); First.reset();
        CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_TRUE(Old.IsValid());
        Pass(R"(
try:
    builtins.value.text
except ReferenceError:
    pass
else:
    raise AssertionError('value containing a retired service remained usable')
)");
        CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_FALSE(Old.IsValid());
    }
    TEST_F(PythonReflectionTests, ExplicitDestructionInvalidatesProxyBeforeCollection)
    {
        Save(); auto* Object = Host->GetEditorSubsystem<DReflectionService>(); ASSERT_NE(Object, nullptr);
        MarkAsGarbage(Object);
        Pass(R"(
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('garbage object accessed')
)");
    }
    TEST_F(PythonReflectionTests, ServiceRemovalDoesNotBroadcastObjectDestruction)
    {
        auto* Retiring = Host->GetEditorSubsystem<DReflectionService>();
        TStrongObjectPtr<DReflectionService> Retained(Retiring);
        bool Observed = false;
        FObjectReferencePurgeRegistration Observer([&](DObject& Object) {
            if (&Object != Retiring) return;
            Observed = true;
            EXPECT_TRUE(IsGarbageCollecting());
        });
        ASSERT_NO_THROW(Host->RemoveEditorSubsystem(DReflectionService::StaticClass())); First.reset(); EXPECT_FALSE(Observed);
        Retained.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_TRUE(Observed);
    }
    TEST_F(PythonReflectionTests, HostCloseReleasesRetainedWrapperRoots)
    {
        Save(); TWeakObjectPtr<DReflectionService> Service(Host->GetEditorSubsystem<DReflectionService>());
        Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
        EXPECT_FALSE(Service.IsValid()); Runtime.Stop();
    }
    TEST_F(PythonReflectionTests, InterpreterRestartRebuildsNativeTypesAndFactories)
    {
        Save(); Runtime.Stop(); ASSERT_TRUE(Runtime.Start());
        const auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        Pass("assert not hasattr(builtins, 'proxy')\na = durin.get_editor_subsystem(durin.types.Durin.PythonFixtures.ReflectionService)\nassert a.get_class() is durin.types.Durin.PythonFixtures.ReflectionService");
    }
    TEST_F(PythonReflectionTests, CancelledJobDoesNotInvalidateSameHostObjects)
    {
        Save(); const auto Pending = Runtime.RunString(Session, "import durin\ndurin.defer(lambda: None)"); ASSERT_TRUE(Pending);
        Runtime.CancelJob(*Pending); Tools->TickScripts();
        EXPECT_EQ(Runtime.GetJobResult(*Pending)->State, EPythonJobState::Cancelled);
        Pass("assert builtins.cached() == builtins.proxy.get_name()");
    }
    TEST_F(PythonReflectionTests, CleanupCannotUseNativeObjectsWithoutActiveJobAdmission)
    {
        Save();
        const auto Result = Run(R"(
class Cleanup:
    def __del__(self):
        try:
            builtins.cached()
        except durin.ScriptAccessError:
            print('cleanup blocked')
        else:
            print('cleanup bypass')
cleanup = Cleanup()
)");
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        EXPECT_NE(Result.Output.find("cleanup blocked"), std::string::npos) << Result.Output;
        EXPECT_EQ(Result.Output.find("cleanup bypass"), std::string::npos);
    }
    TEST_F(PythonReflectionTests, NativeCallContextsRestoreAndDocumentPolicyWinsOverHeadlessWrites)
    {
        auto* World = NewObject<DWorld>(Host.Get(), "ScriptWorld"); World->SetWorldType(EWorldType::Editor);
        ASSERT_NO_THROW(World->InitializeSubsystems());
        auto* Level = NewObject<DLevel>(World, "ScriptLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
        FEditorEngineTestAccess::SetEditorWorld(*Host, World);
        FScopedEditorScriptCall Outer({Host.Get(), Tools->GetService()->GetSessionId()});
        EXPECT_EQ(Outer.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Unavailable);
        {
            FScopedEditorScriptCall Inner({Host.Get(), Tools->GetService()->GetSessionId(), EEditorScriptEditPolicy::NonDocumentReadWrite});
            EXPECT_EQ(FScopedEditorScriptCall::Current(), &Inner);
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Writable);
            bool ReadOnly = true;
            FEditorScriptTargetPolicyRegistration Policy(*Host, *Level, [&] { return ReadOnly ? EEditorScriptTargetAccess::ReadOnly : EEditorScriptTargetAccess::Writable; });
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::ReadOnly);
            ReadOnly = false; EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Writable);
            FEditorScriptTargetPolicyRegistration Conflict(*Host, *Level, [] { return EEditorScriptTargetAccess::Writable; });
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Unavailable);
            Inner.Reject("inner"); EXPECT_FALSE(Outer.GetError().has_value());
        }
        EXPECT_EQ(FScopedEditorScriptCall::Current(), &Outer);
    }
}
