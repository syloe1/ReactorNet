# ADR-002：RPC payload 序列化选型

| 项目 | 内容 |
|---|---|
| ADR 编号 | 0002 |
| 标题 | RPC payload 序列化选型 |
| 状态 | Proposed（随本 PR 合入后转为 Accepted） |
| 日期 | 2026-10-18 |
| 对应 issue | #41 |
| 依赖 | RFC-0001（线格式）、#29（Serializer 接缝） |
| 被依赖 | #42（Codec/Serializer 拆分）、#43（JSON serializer） |
| 决策 | 参考实现的主线是**零依赖手写 JSON**；`Serializer` 接口保持开放，protobuf 作为可选插件 |

---

## 1. 摘要

M2 的 payload 一直是不透明字节，这是刻意的：帧、分发、请求关联都跑通之后，才有足够信息决定 payload 里放什么。现在决定：

1. **参考实现使用零依赖的手写 JSON**。可读、可以直接用 `nc`/`curl` 观察、能完整讲清楚转义与 UTF-8，而且不把第三方运行时引入核心库。
2. **`Serializer` 是独立接口**，protobuf 作为插件接入，而不是主线。`name()` 写进信封，两端可以协商；不匹配时明确失败。
3. **不追求「一种格式打天下」**。选型的核心不是谁的吞吐最高，而是哪一种失败模式最适合这个项目当前阶段。

微基准显示 protobuf wire 在这组小对象上比 JSON 快约 2–3 倍、小约 60%，FlatBuffers 风格的零拷贝读则快一个数量级。**这些数字支持「高频大流量场景应该用二进制」，但不支持「参考实现现在就该以它为主线」**，理由见 §7。

---

## 2. 背景与约束

- 协议层（RFC-0001 §4.3）只规定 envelope，payload 是 `bodyLen` 个不透明字节。序列化格式可以独立演进。
- 核心库当前**没有任何第三方依赖**，CI 在无网环境也能构建。这条约束同样适用于默认构建。
- 项目目标包含教学与可调试性：抓包后应该能肉眼读懂请求/响应。
- 未来需要类型安全注册（#44）、typed stub（#46）和 IDL（#48）。序列化接口必须能承接这些，而不是挡在前面。
- 失败必须是明确的：两端 serializer 不一致时不能「差不多能跑」，要报错。

---

## 3. 候选方案

| 方案 | 编码模型 | 是否需要 schema | 典型代表 |
|---|---|---|---|
| 手写二进制 | 固定布局 / TLV | 隐式（代码即 schema） | 本项目 §4 的 raw |
| JSON | 自描述文本 | 不需要 | 手写 writer/parser |
| MessagePack | 自描述二进制 | 不需要 | msgpack-c |
| protobuf | IDL + varint/TLV | 需要 `.proto` | libprotobuf |
| FlatBuffers | IDL + 可随机访问的布局 | 需要 `.fbs` | flatbuffers |

本 ADR 的微基准对五种方案各写了一个**自包含的参考实现**（`bench/serialization_bench.cpp`）。它们只用于比较编码策略，不是官方运行时的性能测试，见 §4.4。

---

## 4. 微基准

### 4.1 被测对象

同一条记录：

```cpp
struct User {
  uint64_t id = 42;
  int32_t score = 87;
  bool active = true;
  std::vector<int32_t> tags{1, 2, 3, 4};
  std::string name = "alice";
  std::string email = "alice@example.com";
};
```

五种编码：

- **raw binary**：8 字节 id + 4 字节 score + bool + tag 数量 + 4×int32 + 两个长度前缀字符串。
- **JSON**：手写 writer；decoder 是按固定 schema 扫描的 parser（不是通用 JSON parser）。
- **MessagePack**：fixmap 6 项，字符串用 fixstr，整数用正 fixint / uint64，数组用 fixarray。
- **protobuf wire**：字段号 1–6，varint、length-delimited、packed repeated；**不使用 libprotobuf**，手写 wire format。
- **FlatBuffers-style**：手写固定偏移布局；decode 返回指向缓冲区的 view，不复制字符串与数组。

### 4.2 方法与命令

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-bench --target serialization_bench -j
./build-bench/serialization_bench --iterations 200000 --repeat 5
```

- 每个格式先做 round-trip 校验，不一致直接退出（避免「快但错」）。
- encode：重复编码同一条记录，结果长度的校验和写入 `volatile` sink，防止被优化掉。
- decode：对已编码好的缓冲区重复解码，结果字段的校验和写入同一个 sink。
- 每个格式取 5 次运行中最好的一次；表中是操作数/秒（Mop/s，百万次/秒）与编码后字节数。

### 4.3 结果

测试机：AMD Ryzen 7 5800H，Linux 7.0.0-31-generic，g++ 15.2.0，`-O3`（Release）。

| 格式 | encode Mop/s | decode Mop/s | 编码后字节 |
|---|---:|---:|---:|
| raw binary | 8.51 | 23.68 | 60 |
| JSON | 4.46 | 3.36 | 94 |
| MessagePack | 11.18 | 6.26 | 65 |
| protobuf wire | 9.14 | 12.91 | 38 |
| FlatBuffers-style | 36.85 | 150.59 | 62 |

同一命令连续运行 3 次，数量级稳定；绝对值的波动约 ±30%（FlatBuffers-style 的 decode 波动最大，150–286 Mop/s，因为它基本只做几次 `memcpy` 和偏移检查）。

### 4.4 这些数字能说明什么、不能说明什么

**能说明：**

- JSON 的编码体积约为 protobuf wire 的 2.5 倍，decode 慢 3–4 倍。
- 自描述格式（JSON/MessagePack）在 encode 上没有想象中那么吃亏；差距主要在 decode 和体积。
- 零拷贝布局（FlatBuffers-style）的 read 路径确实快一个数量级。
- 手写二进制的 decode 反而比 protobuf 慢，因为这里用的是固定 4/8 字节字段，没有 varint 的紧凑性。

**不能说明：**

- 这些不是 libprotobuf / msgpack-c / flatbuffers 的官方性能。官方实现有 arena、代码生成、SIMD 等优化，数字会不同。
- 记录很小（60–94 字节），大量时间花在函数调用和分配上；大对象、批量数组的结论可能相反。
- FlatBuffers-style 的 decode 是「建立 view」，并没有把数据变成业务对象；一旦业务代码需要 `std::string` 或 `std::vector`，这部分优势会缩小。
- 单线程、热缓存、无网络。真实 RPC 的瓶颈在系统调用、连接调度和业务处理，不在序列化。

结论只用于**方案取舍的方向**，不用于宣称某个库比另一个快多少。

---

## 5. 横向对比

| 维度 | 手写二进制 | JSON | MessagePack | protobuf | FlatBuffers |
|---|---|---|---|---|---|
| 编解码吞吐 | 高 | 中 | 高 | 高 | 读极高 / 写中 |
| 体积 | 小 | 大（+50%–150%） | 小 | **最小** | 中（含 vtable/偏移） |
| 可读性 | 差 | **最好** | 差 | 差（需 schema 与工具） | 差 |
| 抓包直接调试 | 需自带解码器 | **可以** | 需解码工具 | 需 `.proto` + 工具 | 需 `.fbs` + 工具 |
| schema 演进 | 靠纪律 | 天然宽松（字段可缺省） | 天然宽松 | **显式、可演进** | 显式、可演进 |
| 依赖 | 无 | 无（自写） | 库 | 库 + 编译器 | 库 + 编译器 |
| 跨语言 | 自己维护 | 天然 | 好 | **最好** | 好 |
| 学习/教学价值 | 中 | **高**（转义、UTF-8、数字边界） | 中 | 中（IDL 与代码生成） | 中（零拷贝布局） |
| 失败模式 | 布局漂移、静默错位 | 字段缺失/类型错误需显式校验 | 同左 | 缺字段用默认值，可能静默 | 同左 |

---

## 6. 决策

### 6.1 主线：零依赖手写 JSON

- 参考实现默认 serializer 命名为 `json`，`Serializer::name()` 返回 `"json"`。
- 只实现一个**明确的子集**：对象、数组、字符串、布尔、整数、`null` 不需要；不支持浮点（等真有需求再加，避免一开始就踩精度与格式化的坑）。
- 解析器必须有深度上限、明确的错误返回、完整的 `\uXXXX` 与 UTF-8 校验（#43 的验收项）。
- 序列化后的 payload 应该能被 `nc` 直接看到，这是调试价值，不是副产品。

### 6.2 插件：protobuf

`Serializer` 接口与具体格式解耦（#42）：

```cpp
class Serializer {
public:
  virtual ~Serializer() = default;
  virtual std::string name() const = 0;                 // 写进信封
  virtual bool encode(const JsonValue&, std::string*) = 0;
  virtual bool decode(std::string_view, JsonValue*) = 0;
};
```

protobuf 插件实现同一个接口，在**构建时可选**（`-DREACTORNET_WITH_PROTOBUF=ON`），核心库与默认构建不依赖它。双方的 `name()` 写进信封的 serializer 标识；不一致时返回明确错误，而不是尝试猜测。

### 6.3 为什么不是「先写 raw，以后再换」

`RawCodec` 的 payload 目前是调用方自己拼的字节，没有类型信息。JSON 是第一个把「对象 ↔ 字节」这件事显式化的实现；有了它，#44 的类型化注册与 #46 的 typed stub 才有共同的对象模型。先做 raw 再换，等于让两批调用方各写一遍映射。

---

## 7. 为什么不是 protobuf 作为主线

这不是「protobuf 太难」，而是当前约束下它的收益不成立：

1. **依赖代价是结构性的。** 引入 libprotobuf 意味着核心构建需要 protoc、运行时库和版本匹配。当前项目的一个明确卖点是「无第三方依赖，离线可构建」；这个属性对教学项目、CI 和嵌入式交叉编译都重要。作为可选插件，这个代价由需要它的人承担；作为主线，所有人承担。
2. **可调试性与项目目标冲突。** 这个项目要展示协议、线程与 IO，抓包可读是教学设计的一部分。protobuf 的 payload 需要 `.proto` 和工具才能读懂，把「看字节」变成了「先搭工具链」。
3. **当前 payload 是刻意不透明的。** RFC-0001 §4.3 把 payload 留给上层，正是为了在信息最充分时再决定。主线选一个需要 IDL 与代码生成的方案，会把 #44/#48 的复杂度提前压到现在，而不是等类型化需求真正出现。
4. **性能不是当前瓶颈。** §4 的差距在每秒十万级调用、payload 小于 100 字节时才显著；参考实现当前的目标是正确性、可观察性和演进空间。等真实负载证明序列化是瓶颈，`Serializer` 接缝允许在不改协议、不改分发的前提下替换。
5. **演进能力可以后补，耦合不行。** protobuf 的 schema 演进优势在跨团队、跨语言、长期兼容时最值钱。这个项目现在既没有跨团队 API，也没有多语言实现；先接受它的依赖和工具链成本，是为一个尚未出现的问题付费。

**什么时候应该改主线：** 出现稳定的跨语言调用方、payload 结构频繁独立演进、或 profile 明确指向序列化热点。届时按 §6.2 接入 protobuf，`name()` 协商保证新旧两端不会静默误解。

---

## 8. 对后续 issue 的约束

- **#42**：`Codec` 负责帧 + envelope，`Serializer` 负责 payload；拆分的验收标准是既有 RPC 测试行为零变化。信封增加 serializer 标识需要论证兼容性（预留位 vs 升 version），ADR-001/RFC-0001 的纪律同样适用。
- **#43**：JSON 实现必须包含：深度上限、转义、UTF-8 校验、错误位置或可诊断的失败返回、round-trip 与 fuzz 测试。不得引入第三方 JSON 库。
- **#44/#46**：类型化 API 面向 `JsonValue`（后续可泛化为 Serializer 的对象模型），不能直接依赖 JSON 文本细节。
- **默认构建**：`serialization_bench` 是独立 target，不被库或测试链接；默认构建仍然零第三方依赖。

---

## 9. 后果

**正面：**

- 默认构建保持零依赖、离线可用。
- 抓包可读，调试成本低。
- protobuf 有一个明确的接入点，而不是「以后再说」。

**负面 / 风险：**

- JSON 的体积与 decode 开销高于二进制方案；高频场景需要接受或替换。
- 手写 JSON 需要自己保证正确性（转义、UTF-8、深度、数字边界），#43 必须用 fuzz 与边界测试兜住。
- 自描述格式没有编译期 schema 校验，字段名拼写错误只能在运行期发现；类型化 API（#44/#46）需要承担这部分检查。

**重新评估的触发条件：**

- 真实压测中序列化占比超过 20%；
- 出现第二个语言的客户端；
- payload 结构需要独立于代码演进。

---

## 10. 复现

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-bench --target serialization_bench -j
./build-bench/serialization_bench --iterations 200000 --repeat 5
```

输出即 §4.3 的表格；参数 `--iterations` / `--repeat` 可调整。基准源码：`bench/serialization_bench.cpp`。
