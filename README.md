# 重明鸟新闻可信度量化模型（news_qualifier）

## 1. 项目定位

`news_qualifier` 是“重明”事实核查系统中的确定性量化计算组件。它接收已经完成事实拆分、证据标注、NLI 判断、关系维度评分和陪审团评审的结构化 JSON，运行七层 MC-Score 算法，输出逐层可审计结果、四分类概率和拒答决定。

本项目不负责：

- 从一段原始新闻正文中自动拆分声明；
- 调用 DeepSeek、豆包或其他大模型；
- 联网检索证据；
- 自动生成 NLI 标签、因果维度分数或陪审团意见；
- 生成最终面向用户的自然语言回答。

这些工作预计由 ChongMing 的工作 Agent 和预处理 Agent 完成。当前推荐的职责边界为：

| 模块 | 职责 |
| --- | --- |
| 工作 Agent | 事实拆分、证据检索、NLI、关系判断和陪审团意见 |
| 预处理 Agent | 补齐字段、统一同义词、转换枚举并生成 `schema_version=0` JSON |
| `news_qualifier` | 七层确定性计算、概率校准、conformal 拒答和结果序列化 |
| 回答 Agent | 根据 `answer_agent_input` 生成自然语言回答，不绕过拒答决定 |

完整算法理论以 [重明鸟MC-Score优化版算法规范.md](./重明鸟MC-Score优化版算法规范.md) 为准；输入字段以 [算法层输入JSON结构.md](./算法层输入JSON结构.md) 为准。README 只描述当前工程实现，不再维护一套独立的旧公式。

## 2. 当前实现状态

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 第一至第五层前向计算 | 已实现 | 证据簇、内容方向、充分性、逻辑、陪审团及 MC-Score |
| 第六层四分类概率 | 已实现 | 支持四分类温度缩放和 Macro-NLL、Brier、ECE 指标计算 |
| 第七层拒答判定 | 已实现 | 使用 family-level split conformal 预测集；单例才回答 |
| `schema_version=0` JSON/JSONL 读取 | 已实现 | `JsonReader` 支持文件、字符串、流和逐行 JSONL |
| 七层结果 JSON/JSONL 写出 | 已实现 | `JsonWriter` 保留输入并追加全部七层结果 |
| 冻结模型配置读取 | 已实现 | 校验参数范围、权重约束、来源基线和 conformal 阈值 |
| 七层统一运行器 | 已实现 | `QualifierPipeline` 被 predict 调用，未来由 sandbox 复用 |
| `predict` 命令行 | 已实现 | 支持文件输出或纯 JSON 标准输出 |
| CMake 构建 | 已实现 | 生成静态核心库和 `news_qualifier.exe` |
| `sandbox` 命令行 | 已预留，未实现 | 当前会明确返回“P1 尚未实现” |
| 12 参数优化 | 未实现 | 待实现 family 分组交叉验证和参数优化器 |
| 温度拟合器 | 未实现 | 当前只会使用配置中冻结的 `temperature` |
| sandbox 数据集读取与切分 | 未实现 | 待实现 `family_id` 分组和 fit/calib/test 切分 |
| 冻结配置写出 | 未实现 | 当前只有读取器；writer 属于 P1 |
| DeepSeek/豆包 Agent 接口 | 未实现 | 不影响人工 JSON 的本地联调 |
| 持久化自动测试 | 未实现 | 已通过严格编译、CMake 构建和临时端到端冒烟测试 |

当前 P0 可以完成：

```text
一条符合 schema 3 的结构化新闻 JSON
        ↓
加载冻结配置或联调配置
        ↓
完整运行七层算法
        ↓
生成包含七层结果的输出 JSON
```

这只能证明工程链路可以运行。使用未验证配置得到的结果不能证明算法具有现实准确性。

## 3. 当前架构

```text
人工或未来的预处理 Agent
        │ schema_version=0 JSON
        ▼
main.cpp：predict 子命令
        │
        ├─ ModelConfigReader ──► model_config.json
        │
        ├─ JsonReader ─────────► QualifierRequest
        │
        ▼
QualifierPipeline
        ├─ 第一、二层：AtomicContentQualifier
        │                 └─ EvidenceAuthenticityQualifier
        ├─ 第三层：LogicRelationshipQualifier
        ├─ 第四层：Jury
        ├─ 第五层：MultiDimensionalQualifier
        ├─ 第六层：ProbabilityAdjuster
        └─ 第七层：RejectionTrigger
        │
        ▼
QualifierResult
        │
        ▼
JsonWriter
        │
        ├─ output.json
        └─ stdout（只包含 JSON）
```

当前程序内部不创建线程。未来部署为独立子进程时，并发、超时、取消、重启和资源限制应由 ChongMing 父进程管理。

## 4. 目录结构

```text
news_qualifier/
├─ main.cpp
├─ CMakeLists.txt
│
├─ include/                         七层算法接口和共享类型
├─ impl/                            七层算法实现
│
├─ json_reader/
│  ├─ json_reader.h
│  └─ json_reader.cpp
│
├─ json_writer/
│  ├─ json_writer.h
│  └─ json_writer.cpp
│
├─ model_config/
│  ├─ model_config_types.h
│  ├─ model_config_reader.h
│  └─ model_config_reader.cpp
│
├─ runtime/
│  ├─ qualifier_pipeline.h
│  ├─ qualifier_pipeline.cpp
│  ├─ prediction_runner.h
│  └─ prediction_runner.cpp
│
├─ config/
│  ├─ model_config.smoke_finite.json
│  └─ model_config.smoke_infinite.json
│
├─ 算法层输入JSON结构.md
└─ 重明鸟MC-Score优化版算法规范.md
```

计划在 P1 增加：

```text
sandbox/
├─ sandbox_types.h
├─ sandbox_dataset_reader.h/.cpp
├─ family_dataset_splitter.h/.cpp
└─ sandbox_runner.h/.cpp

training/
├─ parameter_optimizer.h/.cpp
├─ temperature_fitter.h/.cpp
└─ test_evaluator.h/.cpp

model_config/
└─ model_config_writer.h/.cpp
```

## 5. 构建

### 5.1 环境要求

- 支持 C++17 的编译器；
- CMake 3.16 或更高版本；
- Windows 可使用 MinGW-w64 或 Visual Studio C++ 工具链；
- 当前代码不依赖第三方 JSON 库。

### 5.2 MinGW 构建

在 PowerShell 或 CMD 中进入项目目录：

```powershell
cd D:\ChongMing\news_qualifier
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel 4
```

生成：

```text
build/news_qualifier.exe
```

### 5.3 Visual Studio 构建

```powershell
cmake -S . -B build
cmake --build build --config Release
```

多配置生成器通常输出：

```text
build/Release/news_qualifier.exe
```

## 6. predict 命令行

### 6.1 写入文件

```powershell
.\build\news_qualifier.exe predict `
  --model config\model_config.smoke_finite.json `
  --input input.json `
  --output output.json
```

输出位置完全由 `--output` 决定。当前 writer 不自动创建父目录，因此指定 `runs\manual\output.json` 前应先创建对应目录。

### 6.2 写入标准输出

```powershell
.\build\news_qualifier.exe predict `
  --model config\model_config.smoke_finite.json `
  --input input.json `
  --stdout
```

使用紧凑 JSON：

```powershell
.\build\news_qualifier.exe predict `
  --model config\model_config.smoke_finite.json `
  --input input.json `
  --stdout `
  --compact
```

`stdout` 只输出机器可解析的 JSON；命令行错误、输入错误、配置错误和算法错误均写入 `stderr`。

### 6.3 帮助

```powershell
.\build\news_qualifier.exe --help
```

### 6.4 退出码

| 退出码 | 含义 |
| ---: | --- |
| `0` | 成功 |
| `1` | 命令行参数或模式错误 |
| `2` | 输入 JSON 语法或业务约束错误 |
| `3` | 冻结模型配置错误 |
| `4` | 算法计算或其他运行错误 |
| `5` | 输出 JSON 写入错误 |

当前 `main.cpp` 一次只处理一个完整 JSON 文件。底层 `JsonReader` 和 `JsonWriter` 已支持 JSONL，但常驻 JSONL 子进程协议尚未接入命令行。

## 7. 输入协议

### 7.1 输入不是原始新闻正文

`predict` 接收的是预处理完成的算法输入，而不是只包含 `title/content` 的原始新闻。输入必须包含工作 Agent 产生的证据标注、关系评分和陪审团意见。

协议使用：

```text
schema_version = 0
snake_case 字段名
UTF-8 JSON
```

总体结构：

```text
QualifierRequest
├─ schema_version                 固定为 0
├─ request_id
├─ event_id
├─ original_text
├─ source
├─ publish_date
├─ event_nature
├─ domain
├─ event_time
├─ seven_elements
├─ atomic_claims[2..5]
│  ├─ claim_id
│  ├─ claim_text
│  ├─ seven_elements
│  ├─ check_question
│  ├─ claim_type
│  ├─ entities[]
│  ├─ canon_refs[]
│  ├─ keywords[]
│  ├─ event_day
│  ├─ time_sensitive
│  └─ evidence[]
├─ logic_edges[1..4]
└─ jury_opinions[]                每条 claim 恰好一个分组
```

完整字段约束和可直接联调的样例见 [算法层输入JSON结构.md](./算法层输入JSON结构.md)。

### 7.2 主要枚举

`source_type`：

```text
official_registry
academic
open_web
social_media
news_media
video_or_github
market_data
```

`nli_label`：

```text
support
refute
neutral
```

`jury_opinions[].opinions[].verdict`：

```text
support
refute
uncertain
```

`logic_edges[].rel`：

```text
temporal
causal
conditional
contrast
coordinate
```

关系维度必须与关系类型严格对应：

| `rel` | `required_dimensions` 和 `dimension_scores` |
| --- | --- |
| `temporal` | `S_time` |
| `causal` | `S_time`, `S_mech`, `S_counter`, `S_confound`, `S_dose` |
| `conditional` | `S_mech`, `S_counter` |
| `contrast` | `S_contrast` |
| `coordinate` | `S_coordinate` |

### 7.3 `family_id` 不属于普通 predict 输入

`family_id` 用于 sandbox 按事件族切分 fit/calib/test，以及在 calib 中计算族级 conformal 阈值。生产 predict 只消费已经冻结的阈值，因此普通 `QualifierRequest` 不需要 `family_id`。

未来 sandbox 的一条 JSONL 记录预计为：

```text
SandboxSample
├─ sample_id
├─ family_id
├─ claim_labels[]
└─ request                       内嵌完整 QualifierRequest
```

## 8. 输出协议

`JsonWriter` 保留完整输入，并在根对象中追加 `qualification_result`：

```text
原始 QualifierRequest 字段
└─ qualification_result
   ├─ algorithm_version
   ├─ input_schema_version
   ├─ config_version
   ├─ calibration_status
   ├─ calculated_at              UTC ISO 8601
   ├─ layer_1_evidence_authenticity
   ├─ layer_2_evidence_sufficiency
   ├─ layer_3_logic_relationship
   ├─ layer_4_jury_evaluation
   ├─ layer_5_multi_dimensional_aggregation
   ├─ layer_6_probability_calibration
   ├─ layer_7_conformal_rejection
   └─ answer_agent_input
```

输出约定：

- 未定义或尚未计算的数值写为 JSON `null`，不伪造成 `0` 或 `0.5`；
- 有限数值以足够往返解析的精度写出；
- JSON 不支持 `Infinity`，无穷 conformal 阈值写成 `"value": null` 和 `"is_infinite": true`；
- 第七层预测集恰为单例时 `should_reject=false`；为空或包含多个类别时拒答；
- `answer_agent_input` 是回答 Agent 使用的精简视图，不替代完整审计结果；
- `event_has_rejected_claims` 只表示至少有一个 claim 被拒答，目前没有额外定义事件级覆盖规则。

四分类稳定标识为：

```text
confirmed_true
mostly_true
insufficient_information
confirmed_false
```

拒答原因稳定标识为：

```text
empty_prediction_set
multiple_candidate_labels
```

## 9. 冻结模型配置

生产推理不会从每条新闻输入中读取算法参数，而是通过 `--model` 加载冻结配置。

冻结配置包含四类数据：

### 9.1 fit 联合优化的 12 个数值

```text
sigmoid_alpha
sufficiency_gamma

causal_weights:
  time
  mechanism
  counterfactual
  confounder
  dose_response

aggregation_weights:
  content
  logic
  sufficiency
  jury
  source
```

约束：

- 五个 `causal_weights` 非负且和为 1；
- 五个 `aggregation_weights` 非负且和为 1；
- `aggregation_weights.logic <= 0.8`。

### 9.2 单独拟合的温度

```text
fitted_calibration.temperature
```

温度不参与上述 12 参数的联合搜索。它应在最佳基础参数产生的 family 分组折外预测上，使用多分类 NLL 单独拟合。

### 9.3 预先冻结的固定参数

```text
reference_cluster_count
time_decay_per_day
epsilon
source_baselines
conformal_error_budget
```

### 9.4 calib 产生的结果

```text
conformal_calibration.threshold
conformal_calibration.is_infinite
conformal_calibration.family_count
conformal_calibration.rank
```

`ModelConfigReader` 会检查版本、数值范围、权重和、温度范围、来源类型完整性，以及 `error_budget`、`family_count` 和 `rank` 是否一致。

### 9.5 联调配置

仓库提供两份仅用于工程联调的配置：

| 文件 | 用途 |
| --- | --- |
| `model_config.smoke_finite.json` | 人工有限阈值，用于观察接受/拒答分支；没有统计意义 |
| `model_config.smoke_infinite.json` | 模拟 calib family 不足；预测集包含全部类别并拒答 |

两份配置均带有：

```json
"calibration_status": "unvalidated"
```

不能将它们作为正式生产模型，也不能用其结果宣称算法准确率。

## 10. 七层算法摘要

本节仅用于理解代码调用关系，完整定义、边界行为、参数优化协议和理论限制见 [重明鸟MC-Score优化版算法规范.md](./重明鸟MC-Score优化版算法规范.md)。

### 第一层：按来源簇计算内容方向

同一原始来源的转载归入一个簇。每簇只保留最强支持、最强反驳和最强中立有效量，避免转载数量线性放大证据。

```text
D_i = (支持强度 - 反驳强度) / (支持强度 + 反驳强度 + epsilon)
S_content,i = sigmoid(alpha * D_i)
```

没有方向性证据时 `D_i=0`、`S_content=0.5`。优化规范已删除旧参数 `beta`。

### 第二层：证据充分性

```text
E_i = sum(max(s_g, r_g, n_g)) / kappa
S_suff,i = 1 - exp(-gamma * E_i)
```

同一来源簇只贡献一次。充分性不会直接改写内容方向。

### 第三层：逻辑关系

因果边使用五个维度：

```text
R_ij = sum(w_k * S_ijk)
L_ij = p_NLI,ij * R_ij
```

没有逻辑边时保持 `logic_available=false`；第五层删除逻辑项，第六层映射时使用中性逻辑量 `0.5`。

### 第四层：带符号陪审团

支持票方向为 `+1`，反驳票为 `-1`，未知票为 `0`。陪审团得分为：

```text
S_jury,i = 0.5 + 0.5 * D_jury,i * Consensus_i
```

没有有效投票时显式记录 `jury_missing=true`，使用 `jury=0.5` 和 `consensus=0`。旧的 `w_R`、`w_U` 已删除。

### 第五层：逐声明 MC-Score

```text
MC_i = weighted_mean(
  content,
  available_logic,
  sufficiency,
  jury,
  source * independence
)
```

使用五个非负 `lambda` 权重。无逻辑边时删除逻辑项并重新归一化，而不是把缺失逻辑当成 0。

### 第六层：四分类概率与温度校准

通过 `MC_i`、方向性充分量和逻辑质量构造 `T/M/I/F` 四类原始质量，再使用多分类温度缩放。温度范围为 `[0.1,5.0]`。

这不是旧版“把单个 MC-Score 做二分类 logit 温度缩放”的实现。

### 第七层：family-level split conformal 拒答

calib 阶段按 `family_id` 聚合真标签非一致性分数，得到冻结阈值 `q_conf`。predict 阶段构造：

```text
Gamma_i = {k : 1 - p_k <= q_conf}
```

唯一判定入口为：

```text
|Gamma_i| == 1  → 接受唯一标签
|Gamma_i| != 1  → 拒答
```

当前实现不再叠加旧版充分性、共识度、独立性和校准概率硬阈值。

## 11. 参数优化与 sandbox 计划

P1 计划使用外层 sandbox JSONL：

```text
SandboxSample
├─ sample_id
├─ family_id
├─ claim_labels[]
│  ├─ claim_id
│  └─ true_label
└─ request
```

训练流程必须严格分离：

```text
fit
  ├─ 按 family 分组交叉验证
  ├─ 搜索 alpha、gamma、w1..w5、lambda1..lambda5
  └─ 使用最佳参数的折外预测拟合温度 T

calib
  └─ 锁定基础参数和 T，只计算 q_conf

test
  └─ 全部参数冻结后只运行一次，不能用于反复挑选参数
```

不应对 12 个基础参数做完整笛卡尔积网格搜索。规范建议：

- `alpha` 和 `gamma` 在对数尺度搜索；
- 两组五维权重通过单纯形或 softmax 参数化；
- 拒绝 `logic_weight > 0.8` 的候选；
- 固定并记录随机种子、family 切分和每次 trial；
- 使用四分类 Macro-NLL 选择基础参数，Macro-BAcc 作为并列次级指标；
- ECE 只作为报告指标。

`conformal_error_budget` 必须在查看 test 结果之前指定。`q_conf`、`family_count` 和 `rank` 是 calib 的计算结果，不是联合搜索参数。

## 12. 工程约定

- C++ 命名空间统一为 `chongming::qualifier`；
- 算法层不直接读取配置文件，配置由 `ModelConfigReader` 读取后通过构造函数注入；
- 宏定义只作为 Config 的默认初值，不是运行时不可覆盖的全局参数；
- `std::optional` 为空表示未定义或尚未计算，与数值零不同；
- 输入分数和概率由 `JsonReader` 做业务范围校验；
- `JsonWriter` 拒绝写出 NaN 和普通 Infinity；
- 标准输出只用于结果 JSON，诊断信息必须进入标准错误流；
- 单个算法进程保持单线程，进程级并发由 ChongMing 管理；
- 回答 Agent 可以组织语言，但不能推翻第七层的拒答决定；
- 修改公式时必须同步更新优化规范、代码、配置协议和测试。

## 13. 当前已知边界

1. 当前没有 Agent 接口，人工输入必须已经符合完整 schema 3；
2. 当前 smoke 配置没有经过真实 sandbox 拟合和校准；
3. 当前 `predict` 一次处理一个 JSON，请求流式 JSONL 尚未接入 `main.cpp`；
4. 非因果关系维度已经进入输入协议，但当前第三层普通关系主要使用 `nli_score`；这些维度如何进一步进入公式仍需单独评审；
5. 当前没有事件级最终标签规则，只输出逐声明决定和 `event_has_rejected_claims`；
6. 尚无持久化单元测试和回归黄金样例；
7. 数学公式闭合不等于现实准确，真实有效性仍需独立 sandbox 数据验证。

## 14. 下一阶段优先级

```text
P0（已完成）
├─ 冻结配置读取
├─ 七层统一流水线
├─ predict 命令行
├─ JSON 输出
└─ CMake 构建

P1（待实现）
├─ sandbox JSONL 读取
├─ family 分组切分
├─ 12 参数优化
├─ 温度拟合
├─ calib conformal 调度
├─ test 指标报告
└─ 候选冻结配置写出

P2（待实现）
├─ 持久化单元测试
├─ 边界黄金样例
├─ JSON/配置回归测试
└─ README 与命令行联调样例

P3（待实现）
├─ 预处理 Agent 适配
├─ 回答 Agent 适配
└─ ChongMing 父子进程协议
```

