# ADR-001：无运行期反射下的 Service/Method 注册方案

| 项目 | 内容 |
|---|---|
| ADR 编号 | 0001 |
| 标题 | 无运行期反射下的 Service/Method 注册方案 |
| 状态 | Accepted |
| 日期 | 2026-10-17 |
| 对应 issue | #17 |
| 依赖 | RFC-0001（线格式）、RFC-002（分层与线程模型） |
| 被依赖 | #30 #44 #45 #46 #48 |
| 决策 | 演进主轴 **A → C**；D 作为插件旁支；E 作为 stretch |

---

## 1. 摘要

C++17 没有运行期反射，也没有可用的编译期成员名枚举。因此「按名字找到方法并调用」这件事必须由两种能力拼出来：

1. **编译期的类型来源**：从成员函数指针推出 `Class` / `Req` / `Resp`；
2. **一次类型擦除**：在注册边界把「带类型的调用」变成一个 `std::function<std::string(std::string_view)>`。

之后整张调用图里只剩帧与字节，分发时只做一次名字查找。**没有运行期反射、没有 `typeid` 分支、没有每方法的虚函数类。**

演进主轴：

```
v0.1（M2）      A 手动 string + bytes        —— 先跑通协议、连接、配对
v1.0（M6）      C 模板注册 + traits          —— 类型从成员函数指针来
可选（M7）      D Protobuf + IDL 插件        —— 证明 Serializer/Codec 接缝开放
stretch（M9）   E 自研 IDL + codegen         —— 名字与方法表全部自动生成
```

**这不是「先用 A 凑合，以后重写」。** A 的注册表条目与 C 的注册表条目是同一个类型：`std::function<std::string(std::string_view)>`。C 只是把「用户手写 thunk」换成「模板替你生成 thunk」，表、分发、协议、连接一行不改。这是本 ADR 的核心论点：**变化被限制在注册前端，运行时接缝保持不变。**

---

## 2. 背景与问题

RPC 服务端需要回答两个问题：

1. **注册时**：用户写 `server.method("GetUser", &UserService::GetUser)`，编译器知道 `UserService`、请求类型、响应类型；这些信息必须被捕获一次。
2. **分发时**：服务端只拿到 `service`、`method` 两个字符串和一段不透明的 payload；此时它**不能**知道类型，也不应该知道。

这两件事之间的落差，就是「类型在哪一刻消失」的问题。它与序列化选择（ADR-002 / #41）、协议帧格式（RFC-0001）都不同：协议层早已决定 payload 不透明，序列化只决定字节↔类型；**本 ADR 决定的是「类型化的调用」如何变成「字节的调用」，以及这张表由谁、在什么时候、以什么代价建立。**

约束来自 RFC-002：

- 注册表在 `start()` 后只读，多 IO 线程并发查表无锁；
- 分发发生在连接 IO 线程；
- handler 抛出的异常不得穿过事件循环；
- 核心网络库不认识任何 RPC 概念。

---

## 3. 决策

| 阶段 | 方案 | 产出 |
|---|---|---|
| M2（#27–#32） | **A**：手动 `string + bytes` | 帧编解码、连接、请求配对、字节级 dispatcher 跑通 |
| M6（#44–#46） | **C**：模板注册 + traits + 一次类型擦除 | 编译期类型推导、类型安全注册、typed stub |
| M7（#41–#43 之后） | **D**：Protobuf 作为可选 Serializer 插件 | 证明 Codec/Serializer 接缝不依赖具体序列化 |
| M9（#48） | **E**：自研 IDL + codegen | 方法名、类型、thunk 自动生成 |

A 不是一次性原型：它的表结构与 C 相同，因此 C 落地时不需要迁移已注册的服务，也不需要改 dispatcher。D 与 E 都是**前端**，分别用 protobuf 描述符和自研 IDL 生成同样的表项。

**实现状态（2026-10-18）：** #30 的 `ServiceRegistry` 已按方案 A 落地（表项是 `std::function<std::string(std::string_view)>`）；#42/#43 已提供 `Serializer` 接缝与通用的 `JsonValue` / JSON 编解码，但它**不是**本 ADR 的类型化前端 C。把 `Req`/`Resp` 结构体映射到 `JsonValue` 的 traits / thunk 仍属于 #44–#46。

---

## 4. 五个方案横向对比

| | A 手动 string+bytes | B `std::function` 类型化 | C **模板注册 + traits** | D Protobuf + IDL | E 自研 IDL + codegen |
|---|---|---|---|---|---|
| 复杂度 | 极低 | 低 | 中高 | 中 | 高 |
| 类型安全 | 无 | 编译期强（构造时） | 编译期强（推导时） | 强 | 强 |
| DX | 差 | 中 | 好 | 好 | 最好 |
| 技术含量 | 中 | 高 | **很高** | 中 | 最高 |
| 第三方依赖 | 无 | 无 | 无 | protobuf | 自研工具链 |
| 类型从哪来 | 用户手写 | 用户手写 | **成员函数指针推导** | `.proto` 生成 | IDL 生成 |
| 类型擦除在哪 | 用户代码 | 用户构造 `std::function` 时 | **模板方法体（thunk）** | protobuf 生成代码 | codegen 生成的 thunk |
| 分发方式 | 字符串查表 | 字符串查表 | 字符串查表 | 服务基类 + 名字查表 | 字符串查表 |
| 定位 | v0.1 起点 | A 的下一步（后半截） | **v1.0 目标形态** | 只做插件 | stretch |

**为什么 B 不是终点**：B 解决了「字节怎么变成一次类型化调用」，但没有解决「类型从哪来」。用户仍然要为每个方法手写：

```cpp
reg["UserService.GetUser"] = [obj](std::string_view body) {
  auto req = json::parse<UserService::GetUserReq>(body);
  auto resp = obj->GetUser(req);
  return json::dump(resp);
};
```

这段代码里的 `GetUserReq`、`obj->GetUser` 已经写了两遍。C 的模板能从头推导出它们，B 不能。B 的价值是作为中间态：它先把表定成 `std::function`，证明类型擦除接缝可用；C 再把这个 thunk 的构造自动化。

---

## 5. 机制：编译期类型 + 一次类型擦除

### 5.1 ① 注册时：编译器从成员函数指针推出类型

```cpp
server.method("GetUser", &UserService::GetUser);
```

`&UserService::GetUser` 的**静态类型**是：

```cpp
User GetUser(const GetUserReq&);   // → User (UserService::*)(const GetUserReq&)
```

模板偏特化可以直接把它拆开：

```cpp
template <class Fn>
struct Signature;  // 主模板不定义：不是支持的签名就在编译期失败

template <class C, class R, class Req>
struct Signature<R (C::*)(const Req&)> {
  using Class = C;
  using Request = Req;
  using Response = R;
  static constexpr bool kConst = false;
};

template <class C, class R, class Req>
struct Signature<R (C::*)(const Req&) const> {
  using Class = C;
  using Request = Req;
  using Response = R;
  static constexpr bool kConst = true;
};
```

于是 `Server::method` 的模板实例化能同时拿到 `Class`、`Req`、`Resp`——**这就是 C++ 里对 Go `reflect.TypeOf` 的替代**，区别是它发生在编译期，产物是代码而不是运行期描述符（§5.5）。

### 5.2 ② thunk：类型在这里还在，擦除也在这里发生

```cpp
template <class C, class R, class Req>
void Server::method(std::string name, C* obj, R (C::*fn)(const Req&)) {
  methods_[std::move(name)] =
      [this, obj, fn](std::string_view body) -> std::string {
        Req req = serializer_.decode<Req>(body);   // 反序列化
        R resp = (obj->*fn)(req);                  // 真正的业务调用
        return serializer_.encode(resp);           // 序列化
      };
}
```

这个 lambda 的闭包类型是编译器生成的唯一类型，带完整类型信息。它被赋给：

```cpp
std::function<std::string(std::string_view)>
```

赋值发生的那一刻，`C`/`Req`/`Resp` 全部消失。之后：

```
            ┌──────────── 类型存在 ────────────┐
注册：      [obj, fn]  →  Req → handler → Resp  →  encode
                                    │
                          ══════════╪══════════  类型擦除
                                    │
分发：      std::string_view  ──────►│──────►  std::string
            只有帧和字节
```

**类型擦除发生在 thunk 的出口——当它被存入 `std::function` 的那一刻，闭包类型与 `C`/`Req`/`Resp` 一起消失；thunk 方法体内类型仍然完整。** 它不是靠继承、不是靠 `typeid`、不是靠运行期注册表里的类型对象。擦除之后，dispatcher 看到的只有 `std::string_view → std::string`。

### 5.3 ③ 运行时表与分发

```cpp
// 注册表（冻结后只读，见 RFC-002 §7.3）
std::unordered_map<std::string,
                   std::function<std::string(std::string_view)>> methods_;

// 分发（连接 IO 线程）
auto it = methods_.find(service + "." + method);
if (it == methods_.end()) return UNKNOWN_SERVICE_OR_METHOD;
std::string payload = it->second(body);   // 零类型信息
```

一次名字查找、一次间接调用。**没有虚函数派发到具体服务类，没有类型信息检查，没有反射。**

实现细节（由 #30 / #44 决定，不在本 ADR 钉死）：

- 扁平表配合 `service + "." + method` 会为每个请求分配一次字符串。可以改成 `services_[svc][method]` 的嵌套表，或 `std::map` + 透明比较器 + `string_view` 查找（C++17 可用），避免拼接分配。无论哪种，**分发路径都不携带类型**。
- 名字里的 `.` 是分隔约定，不是协议的一部分；RFC-0001 的 body 分别携带 `service` 与 `method`，不携带拼接后的名字。

### 5.4 必须支持的签名形态

issue #17 的 R30 要求支持以下五种形态。它们覆盖了从「最常见」到「需要 out 参数/错误码」的主流风格：

| # | 形态 | 类型提取 | 调用方式 | 备注 |
|---|---|---|---|---|
| 1 | `Resp (C::*)(const Req&)` | 偏特化 §5.1 | `Resp r = (obj->*fn)(req);` | 最常见 |
| 2 | `Resp (C::*)(const Req&) const` | 偏特化带 `const` | 同上；`C*` 可调用 const 成员 | `kConst = true` |
| 3 | `void (C::*)(const Req&, Resp*)` | 第一个参数是 `Req`，第二个是 `Resp*` | 默认构造 `Resp`，传指针，编码它 | Go `net/rpc` 风格 |
| 4 | `Status (C::*)(const Req&, Resp&)` | 第一个参数是 `Req`，第二个是 `Resp&`，`Status` 是返回 | 默认构造 `Resp`，传引用；`Status` 映射到错误 | gRPC 风格；`Status` 必须是 RPC 错误类型 |
| 5 | `Resp (*)(const Req&)` | 自由函数偏特化 | `Resp r = fn(req);` | 无对象，thunk 只捕获函数指针 |

实现约束：

- **重载方法**必须用 `static_cast` 显式选择重载，否则模板无法推导；或者由用户显式指定类型。这是 C++17 的固有限制，不是设计缺陷。
- 形态 3/4 的 `Resp` 必须可默认构造——out 参数语义要求先有一个对象。
- 形态 4 的 `Status` 不能是任意类型；它必须能被 RPC 错误模型识别（#39）。不是错误类型的返回值不在支持范围内，用户应改用形态 1 并抛异常或返回结果对象。
- `Req`/`Resp` 必须能被选定的 Serializer 编解码；具体约束见 ADR-002（#41）。**本 ADR 不假设 JSON、不假设 protobuf、不假设任何字段映射方式。**
- `const` 成员形态（2）的 thunk 可以捕获 `C*` 并正常调用；不需要捕获 `const C*`，除非注册 API 明确接受 const 对象（由 #45 决定）。

### 5.5 「Go 的 `reflect.TypeOf` 在 C++ 里对应什么」

这是本 ADR 最值得在面试里讲清楚的一点。

| | Go | C++17 |
|---|---|---|
| 运行期拿到类型描述符 | `reflect.TypeOf(x)` 返回 `reflect.Type`，可枚举字段与方法 | **没有等价物。** `typeid(x)` 只给 `std::type_info`：名字与相等比较，不能枚举字段/方法 |
| 编译期拿到完整类型 | 不适用（Go 是运行期反射） | `decltype(x)`、模板推导；`&C::method` 的静态类型包含全部类型信息 |
| 从「方法」到「类型」 | `MethodByName` 运行期查找 | 模板偏特化从成员函数指针**编译期**拆出 `C`/`Req`/`Resp` |
| 从「类型」到「名字」 | `Type.Name()` / `Method.Name` 运行期可得 | C++17 **不可得**，必须显式传字符串；C++26 静态反射才可能自动获得 |
| 运行期描述符由谁建 | 编译器/运行期自动生成 | **我们自己建**：thunk 就是「这个方法的运行期描述符」——它接受 `string_view` 并完成调用 |

所以答案是：**Go 的 `reflect.TypeOf` 在 C++17 里没有一个运行期对应物；它的能力被拆成「编译期类型推导（模板）」+「手工构造的运行期描述符（thunk）」。** thunk 不是退而求其次的 workaround，而是把「调用一个方法」这件事的完整信息压缩成了一个可存储的闭包——**类型擦除的边界正是运行期描述符的边界。**

---

## 6. 为什么是 A → C，而不是一开始就上 protobuf

先明确：**这不是因为 protobuf 难。** 用 protobuf 更容易——它把三个最难的设计问题一次性替你做完了：

1. **线格式**：varint、长度前缀、字段编号、可选字段、兼容性规则；
2. **类型擦除与分发**：生成的 `Service` 基类 + `CallMethod(name, request, response, done)`，运行期按名字调用；
3. **IDL 与代码生成**：`.proto` → 类型 + 服务描述符 + 客户端 stub。

对一个以「把 RPC 机制搞明白」为目标的项目，这三件事恰恰是**最值得自己写一遍**的部分。protobuf 当主线会得到一个能跑的 RPC，但作者不会因此理解分帧、类型擦除、注册表、错误映射——这些是 RFC-0001、本 ADR、RFC-002 分别定义的机制。

具体理由：

1. **协议已经与序列化解耦。** RFC-0001 的 payload 是不透明字节。这意味着 protobuf 可以在任何时刻作为 Serializer 插件接进来（方案 D），不需要改协议、不需要改连接、不需要改 dispatcher。主线用 A/B/C 不排斥 D。
2. **D 作为插件比 D 作为主线更有说服力。** 「Codec/Serializer 接口是开放的」这个论断，只有存在第二个实现时才能被验证（#42/#43）。把 protobuf 做成可插拔的第二个实现，正好证明接缝；把它做成唯一实现，则接缝从未被检验。
3. **依赖与构建边界。** 核心库是零依赖的（RFC-002 §6.3），protobuf 只能进 `reactornet_rpc`。作为主线，它会把整个 RPC 目标的构建门槛抬高（protoc、版本、生成代码、跨平台）；作为可选插件，它可以默认关闭，不影响任何人跑 example 与测试。
4. **可读性与调试。** JSON/字符串 payload 在 example、日志、抓包里可读；protobuf 二进制在出问题时只能靠工具解码。对一个教学/简历项目，可读性是实际收益。
5. **C++26 反射的叙事。** C 的 traits 是「没有反射时怎么凭空造出反射」的答案；有了 protobuf 生成代码，这个答案被外部工具包办，作者失去讲这件事的机会（§9）。

**什么时候应该选 protobuf 当主线**：生产环境、多语言互通、schema 演进有强约束、团队已有 `.proto` 资产、需要成熟的 codegen 与工具链。这些条件在本项目里都不成立——本项目的目标是理解机制，不是替换 gRPC。ADR 的职责是把 trade-off 写清楚，而不是假装只有一种正确答案。

---

## 7. 类型擦除的三种实现

thunk 的存储形态有三种常见做法。C 方案选用 `std::function`。

| 实现 | 每次调用开销 | 每个方法的存储 | 分配 | 代码量 | 说明 |
|---|---|---|---|---|---|
| **虚函数基类** | 一次虚调用（间接跳转） | 每方法一个具体类对象 + vptr | 每方法一次 `new`（或静态对象） | 中 | 签名固定时清晰；要支持任意 lambda 捕获需要额外模板类，类爆炸 |
| **`std::function`**（选用） | 一次类型擦除调用（间接跳转） | 闭包按 SBO 内联，超出则堆分配 | 捕获大时每方法一次 | 低 | 标准库、可读、够快；SBO 大小由实现定义 |
| **手写 vtable** | 一次函数指针调用 | `void* ctx` + `fn` 两个指针 | 无（可 placement-new 进固定 arena） | 高 | 最小开销、最稳定 ABI；需要自己管理生命周期与对齐 |

选择 `std::function` 的理由：

- 注册发生在启动阶段，每方法一次分配完全可接受；调用开销相对于序列化、系统调用、网络往返可以忽略。
- 手写 vtable 的收益只有在压测证明注册/分发是瓶颈时才成立；ADR-002（#41）会做序列化微基准，届时若真的需要，可以把表项类型隐藏在一个别名后面替换，不改变本 ADR 的结论。
- 虚函数基类为每种签名形态引入一个类模板，且难以优雅地支持自由函数与不同捕获，代码量反而更大。

**「注册 API 天然是模板函数，不是宏」**：宏无法从 `&C::method` 推导 `C`/`Req`/`Resp`，只能把整个表达式再包一层模板（那宏就是多余的）；宏还会让编译错误指向展开点、让调试器丢失真实表达式、让重载解析与 `static_cast` 变得不可读。唯一可能需要宏的是「自动把方法名转成字符串」，但那属于可选的语法糖；C++26 反射可以彻底去掉名字参数（§9）。

---

## 8. 后果（Consequences）

### 8.1 正面

- **A → C 不需要重写运行时。** 表项类型、分发、协议、连接全部不变，C 只替换注册前端。每个阶段都可独立测试、独立演示。
- **类型安全在编译期**：错误的 `Req`/`Resp`、不支持的签名、不可序列化的类型都在编译期报错，而不是运行期 `bad_cast`。
- **分发零反射、零虚函数派发到服务类**：一次名字查找 + 一次擦除调用。
- **注册表冻结后只读**：与 RFC-002 §7.3 的无锁查找天然契合。
- **D 证明接缝**：protobuf 作为第二个 Serializer 接入，不需要改帧层与 dispatcher。
- **面试叙事完整**：从「为什么需要类型擦除」到「擦除发生在哪一行」到「C++26 会怎么取代它」，是一条完整的因果链。

### 8.2 负面

- **C++17 的 traits 代码不平凡**：五种签名形态需要多组偏特化，编译错误信息可能很长。缓解：用 `static_assert` 给出面向用户的诊断（#44）。
- **名字必须显式传**：C++17 拿不到成员名；重载方法还要 `static_cast`。这是语言限制，不是实现遗漏。
- **service 对象的生命周期**：thunk 捕获 `C*`，注册表不拥有对象。对象必须活得比 server 久，或由注册表持有 `shared_ptr`——所有权模型由 #45 决定，本 ADR 只标记这个事实。
- **没有运行期动态注册**：冻结后只读（RFC-002 §7.3）。需要动态注册就得改文档并引入同步，不能悄悄加锁。
- **序列化的类型映射仍然存在**：C 的 thunk 要求 `serializer_.decode<Req>` / `encode(Resp)` 可实例化。JSON 在没有反射时如何把字段映射到结构体，仍属 #44/#46/#48；#43 只交付了通用的 `JsonValue` 对象模型与 JSON 文本编解码，本 ADR 不假装类型映射已经解决。

### 8.3 中性

- 分发路径可能为拼接名字分配一次字符串（§5.3），实现可优化。
- 每方法注册时可能有一次 `std::function` 堆分配；启动期一次性成本。
- D/E 的 codegen 会增加构建步骤；它们是可选/ stretch，不影响默认构建。

---

## 9. C++26 静态反射前瞻

P2996（`^^` 与 `std::meta`）已进入 C++26 草案流程，方向明确：编译期可以枚举类型的成员、成员函数与它们的名字。对本 ADR 的影响：

**可以自动化的部分：**

```cpp
// 伪代码：C++26 风格
template <class T>
consteval auto method_table() {
  auto table = std::make_tuple();
  for (auto m : std::meta::members_of(^^T, std::meta::access_context::current())) {
    if (std::meta::is_function(m) && is_rpc_method(m)) {
      table = std::tuple_cat(table, std::make_tuple(
          std::pair{std::meta::name_of(m), make_thunk(^^T, m)}));
    }
  }
  return table;
}
```

- 方法名不用再手写字符串；
- 重载方法可以按签名区分；
- JSON Serializer 可以遍历 non-static data members 自动生成字段映射（#44/#48 的长期形态；#43 仅提供通用 `JsonValue`）；
- IDL（#48）可以在编译期检查方法签名与协议约束。

**不会改变的部分：**

- **线格式**（RFC-0001）不变：类型信息只影响进程内的注册与编解码，不影响字节布局。
- **类型擦除接缝不变**：反射生成的是同样的 thunk / `std::function` 表项。运行时分发仍然只有名字和字节。
- **注册表冻结与线程模型不变**（RFC-002 §7.3）。
- **错误模型不变**（RFC-002 §8）。

**为什么现在不用**：项目基线是 C++17、GCC 8+；P2996 在主流工具链上的支持仍不普遍，且 C++26 标准尚未全面落地。要求它会把整个项目锁死在一个实验性编译器上。

**设计上的准备**：把「注册前端」与「运行期表」明确分层（本 ADR 的 A/B/C 共用同一张表）。将来反射只是新增一个前端，把同样的表项填进去；**接缝稳定，前端可替换。** 这也是本 ADR 选择 A → C 而不是「等 C++26 一步到位」的理由：等不到，也不该等。

---

## 10. 对下游 issue 的约束

| Issue | 本 ADR 提供的约束 |
|---|---|
| #30 ServiceRegistry | 表项形态是「字节进、字节出」的擦除调用；注册表冻结后只读；分发不携带类型 |
| #41 ADR-002 序列化选型 | C 的 thunk 要求 `decode<Req>` / `encode(Resp)`；D 是其中一个实现；选型必须能支持 §5.4 的类型 |
| #42 Codec/Serializer 拆分 | thunk 依赖 Serializer，不依赖帧层 Codec；帧层仍然是 RFC-0001 的字节布局 |
| #43 JSON Serializer（已交付） | 提供通用 `JsonValue` 与 JSON 文本编解码；**不**提供 `Req`/`Resp` 字段映射，后者留给 #44/#46 |
| #44 类型化注册 | 实现 §5.1 的 `Signature` traits 与 §5.4 的五种签名；错误信息用 `static_assert` 友好化 |
| #45 Service 绑定与生命周期 | 决定注册表是否持有 service 对象；thunk 捕获的 `C*` 必须有效 |
| #46 类型化 Stub | 客户端复用同一套 `Req`/`Resp` traits；不要求运行期反射 |
| #48 IDL 与 codegen | codegen 视为外部前端：生成 `server.method(...)` 调用与类型定义，不改变运行时表 |

**修改流程**：先改本 ADR，再改实现。若 #44 发现 §5.4 的签名形态需要增删，先修订此文档。

---

## 11. 非目标（Non-Goals）

本 ADR **不**做以下事情：

- **不写代码。** 没有 traits 实现、没有注册 API、没有 thunk。
- **不设计 IDL 语法。** 那是 #48；本 ADR 只把 E 定位为 stretch。
- **不决定序列化格式。** JSON / protobuf / 二进制由 ADR-002（#41）决定。
- **不设计注册 API 的最终签名。** §5.2 的 `Server::method` 是机制示意，不是最终接口；参数顺序、返回值、错误处理由 #44 定。
- **不规定 service 对象所有权。** 由 #45 决定。
- **不改协议、不改线程模型。** 分别以 RFC-0001、RFC-002 为准。

---

## 12. 开放问题

| # | 问题 | 为什么现在不定 |
|---|---|---|
| 1 | JSON 无反射时如何映射 `Req`/`Resp` 字段 | #43 只交付通用 `JsonValue`，此问题仍属 #44/#46。可选 ADL `from_json`、显式 traits、或第三方库的宏；本 ADR 不预设 |
| 2 | 注册表是否拥有 service 对象 | 影响 #30/#45 的接口与生命周期；等实际使用形态（栈对象 vs `shared_ptr`）明确后再定 |
| 3 | 形态 4 的 `Status` 具体是什么类型 | 依赖 #39 的错误模型；本 ADR 只要求它可被错误模型识别 |
| 4 | 重载方法的名字约定 | 需要显式名或签名后缀；由 #44 在使用中收敛 |
| 5 | 扁平表 vs 嵌套表、是否避免名字拼接分配 | 启动期与每请求开销的权衡；#44 可用基准测试决定，不影响本 ADR 的结论 |
| 6 | 何时迁移到 C++26 反射前端 | 取决于工具链支持；运行时接缝已为此留好（§9） |

---

## 13. 参考资料

- P2996R* — *Reflection for C++26*（`^^`、`std::meta`、成员枚举）
- P0194 / Reflection TS — 更早的反射提案与实现经验
- Go `reflect` 包文档 — `TypeOf` / `Type.Method` / `MethodByName` 的语义，本 ADR §5.5 的对照
- protobuf 生成代码 — `Service` 基类与 `CallMethod` 的按名分发，方案 D 的参照
- gRPC 生成代码 — 同步/异步 stub 与 `Status` 的形态，§5.4 形态 4 的来源
- `std::function` 的类型擦除实现（SBO、间接调用）— §7 的开销来源
- Klaus Iglberger, *Breaking Dependencies: Type Erasure* — 三种类型擦除实现的对照
- RFC-0001（`docs/rfc/0001-rpc-protocol.md`）— payload 不透明是 D 能作为插件的前提
- RFC-002（`docs/rfc/0002-rpc-layering.md`）— 注册表冻结、线程模型、错误分类

## 修订历史

| 版本 | 日期 | 说明 |
|---|---|---|
| v0.1 | 2026-10-17 | 初稿：A–E 五方案对比、编译期类型推导与 thunk 类型擦除机制、五种签名形态、protobuf 非主线的理由、三种类型擦除实现、C++26 反射前瞻、6 条开放问题 |
| v0.2 | 2026-10-18 | #30/#42/#43 落地后的实现注记：注册表按方案 A 交付；JSON serializer 只提供通用 `JsonValue`，类型化映射仍属 #44/#46 |
