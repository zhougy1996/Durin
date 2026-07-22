# 反射属性视图后续设计参考

本文记录 `FReflectedPropertyView` 的候选演进方向，供后续实现时参考。
它不是当前 API 的规范；已实现行为以
[Reflected Property Editing](../Architecture/ReflectedPropertyEditing.md) 为准。

## 1. 要解决的问题

当前共享 View 已统一属性控件、编辑会话和事务，Level Editor customization
中的直接写入也已完成迁移，但宿主仍需手动遍历属性，部分领域编辑器仍有
重复的字符串属性查找。后续目标是：

- 普通 `DObject` 编辑只需传入对象，不再由面板遍历反射属性；
- Actor、ActorComponent 和资产对象共用同一个对象入口；
- customization 可以重排、隐藏或补充属性，而不重新实现事务；
- 外部不接触不稳定的容器地址和底层 EditTarget 细节；
- 领域语义保留在 setter、adapter、binding 或 customization 中；
- View 只拥有进行中的交互状态，Undo/Redo 历史继续属于 Editor。

## 2. 命名与职责

`FReflectedPropertyView` 是嵌入 Panel 或 Workspace 的即时模式视图：

- `Panel` 表示可停靠的完整区域，例如 `FDetailsPanel`；
- `View` 表示可嵌入的属性界面与临时交互状态；
- `Session` 表示一次编辑的 Begin/Apply/Commit/Cancel；
- `TransactionManager` 表示宿主拥有的全局历史；
- `Editor` 保留给完整资产编辑器或工作区。

因此不建议改名为 `FObjectEditor`，也不建议使用隐藏全局状态的裸
`EditObject(Object)` 函数。

## 3. 候选公开 API

### 3.1 EditObject：默认入口

```cpp
struct FObjectPropertyViewOptions
{
    bool bShowClassName = true;
    bool bShowSearch = true;
    bool bCreatePropertyTable = true;
    std::function<bool(const FProperty&)> Filter;
};

auto FReflectedPropertyView::EditObject(
    const FReflectedPropertyViewContext& Context,
    DObject* Object,
    const FObjectPropertyViewOptions& Options = {}
) -> void;
```

`EditObject()` 应在内部完成：

- 处理空对象和对象切换；
- 遍历继承链上的 `Edit` 属性；
- 合并 `ReadOnly` 状态；
- 生成 DisplayName 和静态数组标签；
- 搜索与 Filter；
- 创建可选的 Property Table；
- 调用对象级 customization；
- 对每个普通字段调用 `EditProperty()`。

Actor 和 ActorComponent 不需要单独的遍历 API：

```cpp
PropertyView.EditObject(Context, SelectedActor);
PropertyView.EditObject(Context, SelectedComponent);
```

### 3.2 EditProperty：保留为公开组合入口

```cpp
auto EditProperty(
    const FReflectedPropertyViewContext& Context,
    DObject* Object,
    FProperty* Property,
    uint32 ArrayIndex = 0,
    const FPropertyViewOptions& Options = {}
) -> bool;
```

`EditProperty()` 不应完全私有。Customization 需要用它完成：

- 调整真实反射属性的顺序；
- 按组展示选定属性；
- 给真实属性替换 Label 或范围；
- 在 Actor 视图中展示 RootComponent 的真实属性。

它是正式的组合接口，不应成为绕过事务系统的临时逃生口。

### 3.3 EditPropertyValue：收回内部

低层递归接口需要调用者传入 `void* Container`、Snapshot 根、Leaf、Path
和 Map Key。错误组合可能产生悬空地址或无效 Undo/Redo。因此建议将以下
接口设为私有：

```cpp
EditPropertyValue(...);
EditArrayProperty(...);
EditMapProperty(...);
CaptureProposedValue(...);
```

外部需要编辑容器中的逻辑值时，使用受控 Binding，而不是构造裸地址。

## 4. Property Binding

Binding 表示一个可编辑逻辑值以及它对应的稳定反射 Snapshot/Path。候选
用法：

```cpp
FReflectedPropertyBinding Binding = PropertyView.BindStringMapValue(
    Material,
    ScalarParametersProperty,
    "Opacity");

PropertyView.EditBoundProperty(Context, Binding, {
    .Label = "Opacity",
    .Minimum = 0.0,
    .Maximum = 1.0,
});
```

Binding 应负责：

- 解析真实 Member/Leaf；
- 生成稳定的数组索引或序列化 Map Key 路径；
- 选择稳定的 Snapshot 根；
- 在 rehash/resize 后重新解析叶子地址；
- 描述 ValueSet 或结构变更；
- 可选地读取继承值和 Override 状态。

当前的 `SubmitStringMapValueEdit()` 与 `SetStringMapEntryEnabled()` 可以视为
Binding 之前的受控过渡 API，后续不应继续为每种容器组合增加平行函数。

## 5. Customization

建议把领域展示扩展注册到共享 View，而不是继续堆积面板类型判断：

```cpp
class IObjectPropertyViewCustomization
{
public:
    virtual auto Draw(
        FReflectedPropertyView& View,
        DObject* Object,
        const FReflectedPropertyViewContext& Context,
        FPropertyViewBuilder& Builder
    ) -> void = 0;
};
```

Builder 可以提供：

```cpp
Builder.AddProperty(Property);
Builder.AddProperty(Property, {.Label = "..."});
Builder.AddBoundProperty(Binding, Options);
Builder.AddCustomRow(Label, DrawCallback);
Builder.HideProperty(Property);
Builder.BeginGroup("Rendering");
```

Customization 注册应支持基类匹配和派生类覆盖，生命周期由 Editor 模块
管理。避免每帧通过字符串查找回调；若未来加入生成元数据，应由代码生成器
验证函数和签名。

## 6. Actor 与 ActorComponent

普通字段全部走 `EditObject()`。Actor Transform 是跨对象的虚拟展示：实际
数据属于 RootComponent，因此应由 Actor customization 组合真实属性：

```cpp
Builder.AddProperty(
    Actor->GetRootComponent(),
    RelativeTransformProperty,
    {.Label = "Transform"});
```

这个展示仍通过 `RelativeTransform` 的语义 Mutation Adapter 调用 setter，
而不是直接写存储。StaticMeshComponent 的材质槽也应使用 customization 或
binding 保留槽名称和 setter 语义。

不要为 Actor 和 ActorComponent 各复制一套属性遍历，也不要把 RootComponent
知识硬编码进通用 `EditObject()`。

## 7. Material Editor

材质参数在存储上是 Map，在界面上是具名参数、继承值和 Override。它不适合
原样展开通用 Map，但可以进一步数据驱动：

```cpp
struct FMaterialParameterDescriptor
{
    std::string_view Name;
    EMaterialParameterType Type;
    double Minimum;
    double Maximum;
    EPropertyPresentation Presentation;
};

static constexpr FMaterialParameterDescriptor Parameters[] = {
    {"Opacity", Float, 0.0, 1.0, Slider},
    {"SpecularStrength", Float, 0.0, 1.0, Slider},
    {"Shininess", Float, 1.0, 256.0, Slider},
    {"BaseColor", Vector, 0.0, 1.0, Color},
};
```

Material Editor 继续拥有预览、分组、继承与 Override UX，但参数 binding、
连续编辑和事务交给 Property View。领域差异不是要删除的特例；应删除的是
重复的 Snapshot、Session、Target 和控件生命周期代码。

## 8. 特例分类

应逐步消除：

- 面板手动遍历所有普通反射属性；
- 直接写属性后手动 Dirty；
- 每个控件重复 Begin/Apply/Commit/Escape；
- 外部构造容器叶子地址和 Map Path；
- 用类类型判断代替 customization 注册；
- 重复的字符串属性查找和参数函数。

必须保留但应集中表达：

- Parent 循环检测；
- Transform 的空间和渲染副作用；
- StaticMesh/Material setter 的资源更新；
- 材质继承和 Override；
- 颜色与普通 Vector 的不同控件；
- 参数范围、单位、只读和资源类型限制。

## 9. 不建议的方案

### Panel 内独立 Undo/Redo

焦点变化会改变撤销语义，面板关闭还可能丢失历史。View 只持有活跃 Session，
提交后的 Transaction 必须进入宿主注入的共享管理器。

### 只有一个裸 EditObject 函数

搜索、资产选择、连续拖动、Escape 和活跃对象都是有状态的。裸函数最终会
依赖 global/static 状态，或要求调用者传入一个等价于 View 的巨大 Context。

### 完全隐藏 EditProperty

Customization 将无法重排或组合真实属性，只能重新实现控件和事务。

### 公开 EditPropertyValue

它暴露不稳定容器地址和过多内部约束，调用者容易破坏 Snapshot 和 Path
不变量。

### 把所有领域语义放进通用 View

通用 View 不应知道 Material、RootComponent 或具体资产工作区。领域语义由
adapter、binding 和 customization 注入。

## 10. 建议实施顺序

1. 增加 `EditObject()`，迁移 Details 的属性遍历、搜索和标签生成。
2. 保持 `EditPropertyValue()` 及容器递归入口为私有实现。
3. 引入最小对象 customization/builder，先承载 Actor Transform 和材质槽。
4. 引入稳定 Binding，替换 string-map 过渡 API 的外部细节。
5. 用参数描述表收敛 Material Editor 重复的标量/颜色/纹理行。
6. 在使用案例稳定后，再评估生成的属性级回调或 customization 元数据。

每一步都应保持现有编辑器可用，并验证连续编辑、Cancel、Undo/Redo、对象切换
和容器地址失效场景。
