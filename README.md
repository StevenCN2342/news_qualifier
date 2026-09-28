# 项目名称：“重明鸟”智能体可信度判定与量化模型

# 项目定位

`news_qualifier` 是“重明”事实核查系统中的确定性量化评分组件。它不负责读取原始新闻、调度大模型、执行事实拆分或进行网络检索，而是接收 ChongMing 中 Agent/NLI 模型生成的结构化中间判断，按照本 README 所述七层算法计算可审计的评分、校准概率和拒答决定。

双方建议按以下职责边界协作：

| 模块 | 主要职责 |
| --- | --- |
| ChongMing | 原始新闻读取、事实拆分、Agent 通路选择、证据检索、NLI/关系判断、生成陪审团原始意见、子进程并发与生命周期管理 |
| news_qualifier | 证据可靠性计算、内容/充分性/逻辑评分、陪审团意见融合、多维综合评分、概率校准、拒答判定 |

ChongMing 提供的是模型或 Agent 的局部判断，例如 NLI 标签、因果概率和陪审团投票；最终的公式计算及量化结论由 `news_qualifier` 完成。

# 当前实现状态

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 第一至第五层算法 | 已实现 | 包含证据、原子声明、逻辑、陪审团和多维评分 |
| 第六层概率校准 | 已实现 | 支持温度缩放和 ECE；温度训练流程尚未确定 |
| 第七层拒答判定 | 已实现 | 支持保形阈值和四条显式规则；非一致性分数定义尚待评审 |
| JSON/JSONL 输入解析 | 已实现 | 当前协议版本为 `schemaVersion: 1` |
| 结果 JSON 序列化 | 未实现 | 输出协议仍是提案，等待 ChongMing 维护者评审 |
| 子进程入口 `main.cpp` | 未实现 | 待输入输出协议冻结后实现 |
| 构建系统 `CMakeLists.txt` | 未实现 | 上传和部署前需要补充 |
| 持久化单元测试 | 未实现 | 当前代码已做严格编译与冒烟验证，但测试尚未纳入仓库 |

# 项目架构

计划中的服务端调用链如下：

```text
ChongMing
├── 事实拆分与证据检索 Agent
├── NLI、关系判断与陪审团意见
└── 子进程调度器
        │ JSONL / stdin
        ▼
news_qualifier（独立、单线程子进程）
├── JsonReader
├── 第一至第五层量化
├── 第六层概率校准
├── 第七层拒答判定
└── JsonWriter（待实现）
        │ JSONL / stdout（待实现）
        ▼
ChongMing 保存、展示或进入人工复核
```

并发、超时、取消和子进程数量由 ChongMing 控制；单个 `news_qualifier` 进程按顺序处理 JSONL 请求，不在算法内部创建线程。

当前仓库的真实目录如下：

```text
news_qualifier/
├── json_reader/
│   ├── json_reader.h
│   └── json_reader.cpp
├── include/
│   ├── qualifier_types.h
│   ├── atomic_content_qualifier.h
│   ├── evidence_authenticity_qualifier.h
│   ├── logic_relationship_qualifier.h
│   ├── jury.h
│   ├── multi_dimensional_qualifier.h
│   ├── probability_adjuster.h
│   └── rejection_trigger.h
├── impl/
│   ├── atomic_content_qualifier.cpp
│   ├── evidence_authenticity_qualifier.cpp
│   ├── logic_relationship_qualifier.cpp
│   ├── jury.cpp
│   ├── multi_dimensional_qualifier.cpp
│   ├── probability_adjuster.cpp
│   └── rejection_trigger.cpp
└── README.md
```

计划在协议冻结后增加：

```text
runner/main.cpp
json_writer/json_writer.h
json_writer/json_writer.cpp
tests/
CMakeLists.txt
```

# 输入协议（已实现，schemaVersion 1）

`json_reader` 支持一个完整 JSON 文档，也支持每行一个独立请求的 JSONL。服务端常驻子进程推荐使用 JSONL，以便逐条处理请求并限制内存占用。

根对象结构为：

```text
QualifierRequest
├── schemaVersion       必填，目前只能为 1
├── requestId           必填，非空字符串
├── claims              必填，可为空数组
│   └── evidence        每条声明内必填，可为空数组
├── relations           可选，缺失等价于空数组
└── juryOpinions        可选，缺失等价于空数组
```

## 1.根字段

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `schemaVersion` | integer | 是 | 当前只能为 `1` |
| `requestId` | string | 是 | 非空，由父进程生成，用于关联未来响应 |
| `claims` | array | 是 | 声明 ID 在单个请求中必须唯一 |
| `relations` | array | 否 | 引用的 Claim ID 必须存在 |
| `juryOpinions` | array | 否 | 每个 Claim 最多出现一个意见分组 |

## 2.claims与evidence

每条 Claim 表示事实拆分后的原子声明：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `id` | string | 是 | 非空且在本请求内唯一 |
| `text` | string | 是 | 非空的原子声明正文 |
| `keywords` | string[] | 是 | 声明关键词；重复值读取后自动去重 |
| `eventDay` | integer | 否 | 事件日期的整日序号；声明与证据必须使用同一纪元 |
| `timeSensitive` | boolean | 否 | 缺失时为 `false` |
| `evidence` | array | 是 | 当前声明的候选证据，可为空 |

每条 Evidence 只属于其外层 Claim：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `id` | string | 是 | 非空；同一 Claim 内必须唯一 |
| `originalSourceId` | string | 否 | 原始来源簇 ID；缺失时按独立证据处理 |
| `sourceType` | enum string | 否 | 缺失时来源基线可靠性为 0 |
| `keywords` | string[] | 是 | 证据关键词 |
| `eventDay` | integer | 否 | 证据事件日期的整日序号 |
| `nliLabel` | enum string | 是 | NLI 模型的支持/反驳/中立判断 |
| `nliConfidence` | number | 是 | NLI 置信度，必须位于 `[0,1]` |

`sourceType` 当前接受：

```text
OfficialRegistry
Academic
OpenWeb
SocialMedia
NewsMedia
VideoOrGitHub
MarketData
```

`nliLabel` 当前接受：

```text
Support
Refute
Neutral
```

## 3.relations

关系在外部协议中通过稳定的 Claim ID 建立连接；`JsonReader` 会将 ID 转换成 C++ 算法内部使用的数组下标。

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `leftClaimId` | string | 是 | 左侧声明 ID |
| `rightClaimId` | string | 除 `Not` 外必填 | 右侧声明 ID |
| `type` | enum string | 是 | 关系类型 |
| `parsingSucceeded` | boolean | 否 | 缺失时为 `true`；失败时逻辑层回退到多数投票 |
| `nliScore` | number | 条件必填 | 时间、条件、转折、并列关系解析成功时必填 |
| `causalProbability` | number | 条件必填 | 因果关系解析成功时必填 |
| `counterfactualProbability` | number | 条件必填 | 因果关系解析成功时必填 |

所有关系概率必须位于 `[0,1]`。`type` 当前接受：

```text
Temporal
Conditional
Contrast
Parallel
And
Or
Not
Implication
Causal
```

## 4.juryOpinions

陪审团输入是 Agent 的原始投票，不是最终陪审团分数。`news_qualifier` 将继续计算信任权重、归一化票数、熵、一致性分和陪审团分。

```text
juryOpinions[]
├── claimId
└── opinions[]
    ├── verdict
    ├── hybridRetrievalScore
    ├── confidence
    └── completedWithoutError
```

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `claimId` | string | 是 | 必须引用本请求中的 Claim |
| `verdict` | enum string | 是 | `Support`、`Refute` 或 `Uncertain` |
| `hybridRetrievalScore` | number | 是 | 必须位于 `[0,1]` |
| `confidence` | number | 是 | 必须位于 `[0,1]` |
| `completedWithoutError` | boolean | 是 | Agent 是否无错误完成推理 |

## 5.完整请求示例

```json
{
  "schemaVersion": 1,
  "requestId": "req-001",
  "claims": [
    {
      "id": "c1",
      "text": "某地区今年的粮食产量增长了10%",
      "keywords": ["地区", "今年", "粮食产量", "增长10%"],
      "eventDay": 20350,
      "timeSensitive": true,
      "evidence": [
        {
          "id": "e1",
          "originalSourceId": "source-001",
          "sourceType": "OfficialRegistry",
          "keywords": ["粮食产量", "增长10%"],
          "eventDay": 20349,
          "nliLabel": "Support",
          "nliConfidence": 0.92
        }
      ]
    }
  ],
  "relations": [],
  "juryOpinions": [
    {
      "claimId": "c1",
      "opinions": [
        {
          "verdict": "Support",
          "hybridRetrievalScore": 0.85,
          "confidence": 0.90,
          "completedWithoutError": true
        }
      ]
    }
  ]
}
```

JSONL 模式要求每个非空行都是一个完整根对象，不支持一条记录跨越多行。空行会被忽略；语法或业务字段不合法时抛出 `JsonReadError`，错误信息包含来源名、行列号或 JSON 字段路径。

# 输出协议（提案，尚未实现）

输出协议必须与 ChongMing 维护者共同评审后才能冻结。当前建议每条输入对应一条输出，并原样返回 `schemaVersion` 和 `requestId`。预计结果至少包含：

```text
schemaVersion
requestId
ok
result
├── claimEvaluations
├── logicEvaluations
├── juryEvaluations
├── dimensions
├── mcScore
├── label
├── calibratedProbability
└── rejection
    ├── shouldReject
    ├── reasons
    └── suggestedMessage
```

建议错误响应至少包含稳定错误码和可读信息：

```json
{
  "schemaVersion": 1,
  "requestId": "req-001",
  "ok": false,
  "error": {
    "code": "INVALID_INPUT",
    "message": "输入字段不符合协议"
  }
}
```

上述字段名称、是否返回全部中间量、浮点精度和错误后是否继续处理下一行，均属于待评审内容，当前代码没有实现结果 JSON 序列化。

# 第六、七层集成待确认事项

以下问题不影响当前公式实现，但会影响最终进程协议、训练/校准流程和部署方式：

1. 温度参数 $T$ 由谁训练、采用什么损失函数及优化方法；
2. 训练集、验证集、校准集和测试集如何划分，是否需要按模型版本分别保存；
3. `temperature`、`epsilon`、拒答阈值从配置文件、命令行还是环境变量加载；
4. ECE 的默认分箱数量及评估报告格式；
5. 新样本非一致性分数的明确定义；
6. 保形校准分数由谁生成和维护，阈值 $\tau$ 离线预计算还是进程启动时计算；
7. 校准数据缺失、版本不匹配或阈值不可用时，是拒答、报错还是进入人工复核；
8. 第六、七层参数属于每个请求、每个模型版本，还是整个评分器进程的公共配置。

在这些问题确认前，`json_reader` 暂不接收第六、七层运行参数；当前实现通过 C++ 配置对象接收它们。

# 工程约定

- C++ 命名空间为 `chongming::qualifier`。
- 算法结果使用 `std::optional` 区分“尚未计算”和数值 0。
- 完成计算但方向信息不足时，方向分数取 0.5；完成计算但质量条件不足时，质量分数取 0。
- 线协议中的概率和评分必须位于 `[0,1]`，`JsonReader` 对越界输入明确报错；算法内部仍保留防御性边界保护，只有公式明确要求时才把裁剪作为正式计算步骤。
- `stdout` 将来只用于机器可解析的 JSONL；诊断信息应写入 `stderr`。
- 单个评分器进程保持单线程；父进程负责并发、超时、取消、重启和资源限制。
- JSONL 到达 EOF 时评分器应正常退出；完整退出码和信号约定待 `main.cpp` 实现时确定。

# 代码评审重点

首次上传 GitHub 后，建议重点评审：

1. ChongMing 能否稳定生成输入协议中的全部字段；
2. 字段由哪个 Agent 或模型负责，以及缺失时如何处理；
3. 当前输入枚举是否足以覆盖实际来源和关系类型；
4. 第六、七层的训练数据、非一致性分数和配置归属；
5. 输出是否需要全部中间分数，还是只需要最终结果和审计摘要；
6. `schemaVersion: 1` 冻结后如何管理不兼容变更；
7. 本地 JSONL 固定样例和端到端预期结果。

# 七层算法研究成果

`include/` 中的 `.h` 文件声明数据结构和接口，`impl/` 中的 `.cpp` 文件提供对应实现。以下七层算法论述为研究成果，工程实现和协议调整不得直接改写算法公式；如研究结论发生变化，应单独评审并同步更新代码、测试与本文档。

下文统一使用以下记号：原子声明记为 $c_i$，其第 $j$ 条证据记为 $e_{ij}$，全部原子声明的集合记为 $C$，逻辑关系集合记为 $R$，证据集合记为 $\mathcal{E}$。所有分数均应限制在 $[0,1]$ 内。

### (1)原子声明内容事实性评分

第一步计算每条证据的可靠性，并据此汇总原子声明的支持强度与反驳强度。

#### 1.来源可靠性

证据 $e_{ij}$ 的可靠性为：

$$
r_{ij}=r_{\mathrm{base}}(s_{ij})\cdot\rho_{ij}\cdot\tau_{ij}
$$

- $r_{\mathrm{base}}(s_{ij})$：来源类型的基线可靠性。
- $\rho_{ij}$：证据与声明的主题相关性。
- $\tau_{ij}$：证据的时效性。

建议的来源基线分数如下：

| 来源类型 | $r_{\mathrm{base}}$ |
| --- | ---: |
| 语料库或官方注册库 | 0.90 |
| 学术来源 | 0.88 |
| 开放网络 | 0.72 |
| 社交媒体 | 0.72 |
| 新闻媒体 | 0.68 |
| 视频或 GitHub | 0.65 |
| 市场数据 | 0.60 |

设声明和证据的关键词集合分别为 $K_{c_i}$ 与 $K_{e_{ij}}$，主题相关性采用包含度计算：

$$
\rho_{ij}=\frac{|K_{c_i}\cap K_{e_{ij}}|}
{\min(|K_{c_i}|,|K_{e_{ij}}|)}
$$

当 $\rho_{ij}<\theta_\rho$ 时过滤该证据，建议 $\theta_\rho=0.2$。实现时若任一关键词集合为空，应直接将 $\rho_{ij}$ 置为 0，避免除零。

对于时效敏感的事件，时效性采用指数衰减：

$$
\tau_{ij}=\exp\left(-\lambda\left|t_{e_{ij}}-t_{\mathrm{claim}}\right|\right)
$$

其中 $\lambda$ 为按天计算的衰减速率，建议 $\lambda=0.01$；对于非时效事件，令 $\tau_{ij}=1$。

#### 2.NLI证据关系判定

自然语言推理模型输出：

$$
(y_{ij},p_{ij}),\qquad
y_{ij}\in\{S,R,N\},\quad p_{ij}\in[0,1]
$$

其中 $S$、$R$、$N$ 分别表示支持、反驳和中立，$p_{ij}$ 为该判定的置信度。

#### 3.来源独立性折扣

将具有相同原始来源的证据归入同一来源簇。同一簇内每条证据的折扣为：

$$
\delta_{ij}=\frac{1}{\sqrt{|\operatorname{cluster}(e_{ij})|}}
$$

因此，同一来源簇包含 $n$ 条证据时，每条证据的权重为 $1/\sqrt{n}$，该簇的总贡献按 $\sqrt{n}$ 增长，而不是随转载数量线性增长。

#### 4.内容事实性得分

先分别计算有效支持强度和有效反驳强度：

$$
A_i^+=\sum_j\delta_{ij}\mathbf{1}[y_{ij}=S]p_{ij}r_{ij}
$$

$$
A_i^-=\sum_j\delta_{ij}\mathbf{1}[y_{ij}=R]p_{ij}r_{ij}
$$

再计算原子声明的内容事实性分数。为使“没有证据”保持中性，并避免除零，使用分段定义：

$$
\operatorname{ContentScore}(c_i)=
\begin{cases}
0.5, & A_i^++A_i^-<\varepsilon,\\[4pt]
\sigma\left(\alpha\dfrac{A_i^+}{A_i^++A_i^-}-\beta\right),
& A_i^++A_i^-\ge\varepsilon.
\end{cases}
$$

其中 $\sigma(x)=1/(1+e^{-x})$，建议初始值为 $\alpha=4$、$\beta=2$、$\varepsilon=10^{-8}$。支持证据占优时分数趋近 1，反驳证据占优时趋近 0；证据势均力敌或完全缺失时分数为 0.5。证据是否足以得出结论由第二步单独判断。

<!-- 算法审阅待确认：按当前公式及 α=4、β=2 计算，全部为支持证据时 ContentScore=σ(2)≈0.881，全部为反驳证据时 ContentScore=σ(-2)≈0.119；由于公式使用支持证据占比，继续增加同方向证据也不会使分数进一步趋近 1 或 0。请确认应修改“趋近 1/0”的文字表述，还是调整 α、β 或评分公式。 -->

### (2)证据充分性评分

第二步用于区分“缺少证据”和“已有证据证明声明为假”。原子声明 $c_i$ 的证据充分性分数为：

$$
\operatorname{SufficiencyScore}(c_i)=
1-\exp\left(-\gamma\sum_j\delta_{ij}r_{ij}\right)
$$

建议 $\gamma=0.5$。独立且可靠的证据越多，分数越接近 1；新增证据带来的边际增益逐渐减小。

<!-- 算法审阅待确认：SufficiencyScore 当前累加全部 δ_ij·r_ij，没有排除 NLI 判定为中立（y_ij=N）的证据；同时，同一来源簇的 n 条证据按当前平方根折扣仍会产生 √n 的总贡献。因此，大量中立证据或同源转载也能提高充分性。请确认充分性是否应只统计支持/反驳证据，以及是否需要对同一来源簇的充分性贡献设置上限。 -->

当

$$
\operatorname{SufficiencyScore}(c_i)<\theta_{\mathrm{suff}}
$$

时，无论 `ContentScore` 为何，都将该声明标记为“信息不足”。建议 $\theta_{\mathrm{suff}}=0.3$。

<!-- 算法审阅待确认：这里规定单个声明的 SufficiencyScore<0.3 时必须标记为“信息不足”，但第五步又仅按整体 MCScore 区间生成四标签，第七步使用的也是声明平均后的 S_suff。请确认该逐声明门槛是否应覆盖 MCScore 标签，以及关键声明证据不足时是否必须让整篇新闻进入“信息不足”或拒答。 -->

### (3)逻辑关系真实性评分

第三步检查原子声明之间的时间、因果、条件、转折和并列关系。对于关系三元组 $(c_i,c_j,rel)$，将两条声明的证据作为前提，将待验证关系作为假设，使用 NLI 模型计算：

$$
\operatorname{LogicScore}(c_i,c_j,rel)=
\operatorname{NLI}\left(
E_{c_i}\land E_{c_j},
\operatorname{Relation}(c_i,c_j,rel)
\right)
$$

对于因果关系，还需进行反事实修正：

$$
\operatorname{CausalConfidence}(c_i\rightarrow c_j)=
p_{\mathrm{causal}}\left(1-p_{\mathrm{counterfactual}}\right)
$$

$p_{\mathrm{counterfactual}}$ 表示“移除原因事件后，结果事件仍会发生”的估计概率。该概率越高，因果关系可信度越低。

复杂逻辑结构按以下规则聚合：

| 逻辑关系 | 聚合公式 |
| --- | --- |
| 合取 AND | $S(c_1\land c_2)=\min(S(c_1),S(c_2))$ |
| 析取 OR | $S(c_1\lor c_2)=\max(S(c_1),S(c_2))$ |
| 否定 NOT | $S(\neg c_1)=1-S(c_1)$ |
| 蕴含 IMPLICATION | $S(c_1\rightarrow c_2)=S(\neg c_1\lor c_2)$ |
| 因果 CAUSAL | $S(c_1\xrightarrow{\mathrm{causal}}c_2)=S(c_1\land c_2)\cdot\operatorname{CausalConfidence}$ |

当逻辑解析失败，或参与关系计算的原子声明处于不确定状态时，系统回退到原子声明的多数投票结果。

### (4)多智能体德尔菲陪审团裁决融合

第四步由严格法理者、开放网络实用主义者、因果怀疑论者、反操纵与阴谋叙事探测者等角色独立审议，再通过信任加权投票融合结论。

智能体 $p$ 的信任分数为：

$$
\operatorname{trust}_p=
0.4s_{\mathrm{hybrid},p}+0.4c_p+0.2I_{\mathrm{no\ error},p}
$$

其中 $s_{\mathrm{hybrid},p}$ 为混合检索平均得分，$c_p$ 为智能体自身置信度，$I_{\mathrm{no\ error},p}$ 表示推理是否无错误完成。

设智能体的判定 $y_p\in\{S,R,U\}$，其中 $U$ 表示不确定。各类别的未归一化票数为：

$$
V_i(v)=\sum_{p\in P}\operatorname{trust}_p\,c_p\,\mathbf{1}[y_p=v]
$$

令总有效票重为 $Z_i=\sum_{u\in\{S,R,U\}}V_i(u)$。当 $Z_i>0$ 时，将票数归一化：

$$
q_i(v)=\frac{V_i(v)}{Z_i}
$$

当 $Z_i=0$ 时，不计算陪审团分数，直接将该声明标记为“不确定”并交由第七步处理。

<!-- 算法审阅待确认：当 Z_i=0 时，本声明没有 ConsensusScore；但第五步仍要求对全部声明的 ConsensusScore 求平均。请确认聚合时应跳过该声明、将其一致性记为 0，还是直接触发拒答。 -->

陪审团一致性由归一化熵计算：

$$
\operatorname{ConsensusScore}(c_i)=
1-\frac{H(q_i)}{\log_2 3},\qquad
H(q_i)=-\sum_v q_i(v)\log_2 q_i(v)
$$

约定 $0\log_2 0=0$。所有智能体意见一致时得分为 1，三类意见均匀分布时得分为 0。

<!-- 算法审阅待确认：如果所有智能体都选择 U（不确定），则 q_i(U)=1、ConsensusScore=1，因而不会触发第七步的“陪审团严重分歧”条件；但这实际表示全体智能体均无法作出真伪判断。请确认是否需要单独计算“不确定票占比”，并在全体或多数选择 U 时触发拒答。 -->

支持、反驳和不确定的效用权重分别取 $w_S=1$、$w_R=0$、$w_U=0.5$，最终裁决分数为：

$$
\operatorname{JuryScore}(c_i)=
\operatorname{ConsensusScore}(c_i)
\sum_{v\in\{S,R,U\}}w_vq_i(v)
$$

<!-- 算法审阅待确认：本节将 JuryScore 定义为多智能体的最终裁决分数，但第五步的 MCScore 公式没有使用 JuryScore，陪审团的支持/反驳裁决不会影响综合可信度。请确认 JuryScore 应以独立维度加入 MCScore、用于修正 ContentScore，还是仅作为展示字段。 -->

### (5)多维评分输出与综合可信度

第五步先输出可解释的分项结果，再计算综合可信度。

| 评分维度 | 计算公式 |
| --- | --- |
| 内容事实性 | $S_{\mathrm{content}}=\frac{1}{|C|}\sum_i\operatorname{ContentScore}(c_i)$ |
| 逻辑关系 | $S_{\mathrm{logic}}=\frac{1}{|R|}\sum_{(i,j,rel)\in R}\operatorname{LogicScore}(c_i,c_j,rel)$ |
| 证据充分性 | $S_{\mathrm{suff}}=\frac{1}{|C|}\sum_i\operatorname{SufficiencyScore}(c_i)$ |
| 来源可靠性 | $S_{\mathrm{source}}=\frac{1}{|\mathcal{E}|}\sum_{i,j}r_{ij}$ |
| 来源独立性 | $S_{\mathrm{indep}}=1-\frac{\sum_{i,j}(1-\delta_{ij})}{|\mathcal{E}|}$ |
| 陪审团一致性 | $S_{\mathrm{consensus}}=\frac{1}{|C|}\sum_i\operatorname{ConsensusScore}(c_i)$ |
| 不确定性 | $S_{\mathrm{uncertain}}=1-S_{\mathrm{suff}}S_{\mathrm{consensus}}$ |

<!-- 算法审阅待确认：S_uncertain 在此被定义为输出维度，但后续 MCScore、概率校准和拒答条件均未直接使用它。请确认该分数是否只用于展示，或应进入综合评分/拒答规则。另请明确当 |C|=0、|R|=0 或 |E|=0 时各平均值的默认值或回退策略，以避免除零。 -->

来源质量采用可靠性与独立性的乘法耦合，避免多个高可靠但实际同源的转载内容虚增分数。综合可信度为：

$$
\operatorname{MCScore}=
\lambda_1S_{\mathrm{content}}+
\lambda_2S_{\mathrm{logic}}+
\lambda_3S_{\mathrm{suff}}+
\lambda_4S_{\mathrm{source}}S_{\mathrm{indep}}
$$

其中 $\lambda_k\ge 0$ 且 $\sum_{k=1}^{4}\lambda_k=1$。建议初始值为：

$$
(\lambda_1,\lambda_2,\lambda_3,\lambda_4)=(0.35,0.25,0.20,0.20)
$$

根据 `MCScore` 输出四标签判定：

<!-- 算法审阅待确认：请明确四标签判定与第二步“证据不足强制标记”和第七步“拒答判定”的执行优先级。建议确认最终输出流程是先计算 MCScore 标签、再由证据门槛或拒答规则覆盖，还是先门控后分配分数标签。 -->

| 分数范围 | 标签 | 含义 |
| --- | --- | --- |
| $[0.85,1.00]$ | 确认为真 | 核心声明有充分证据支持，且逻辑关系成立 |
| $[0.55,0.85)$ | 大体为真但有瑕疵 | 大部分声明成立，但存在局部事实或逻辑问题 |
| $[0.30,0.55)$ | 信息不足 | 关键声明缺乏充分证据，暂时无法确定真伪 |
| $[0,0.30)$ | 确认为假 | 核心声明被反驳，或逻辑关系存在根本错误 |

### (6)概率校准

第六步使用温度缩放将原始 `MCScore` 转换为更接近真实正确率的校准概率。为避免数值溢出，先执行边界裁剪：

$$
\widetilde{p}=\operatorname{clip}(\operatorname{MCScore},\varepsilon,1-\varepsilon)
$$

然后计算：

$$
p_{\mathrm{calibrated}}=
\sigma\left(\frac{\operatorname{logit}(\widetilde{p})}{T}\right),\qquad
\operatorname{logit}(p)=\ln\frac{p}{1-p}
$$

温度参数 $T>0$ 在验证集上学习。可使用预期校准误差 ECE 评估校准质量：

$$
\operatorname{ECE}=
\sum_{b=1}^{m}\frac{|B_b|}{N}
\left|\operatorname{acc}(B_b)-\operatorname{conf}(B_b)\right|
$$

$B_b$ 为第 $b$ 个置信度分箱，$\operatorname{acc}(B_b)$ 为该分箱的实际正确率，$\operatorname{conf}(B_b)$ 为平均预测置信度。`p_calibrated` 用于后续风险控制，不应再把未经校准的 `MCScore` 直接解释为概率。

### (7)拒答判定

第七步通过保形预测和显式安全条件决定“回答”或“拒答”。设校准集包含 $n$ 个非一致性分数 $s_1,\ldots,s_n$，用户允许的错误预算为 $\alpha$，建议取 $0.05$ 至 $0.10$。将分数升序排列后，保形阈值取：

<!-- 算法审阅待确认：符号 α 已在第一步表示 ContentScore 的 sigmoid 参数（建议值 4），此处又表示保形预测错误预算（建议值 0.05～0.10），两者含义和取值冲突。请确认是否将错误预算改记为 α_conf、η 或其他独立符号，并同步加入参数表。 -->

$$
k=\min\left(n,\left\lceil(n+1)(1-\alpha)\right\rceil\right),\qquad
\tau=s_{(k)}
$$

新样本的非一致性分数高于 $\tau$ 时触发拒答。此外，满足以下任一规则也应直接拒答：

1. $S_{\mathrm{suff}}<0.3$：证据不足；
2. $S_{\mathrm{consensus}}<0.4$：陪审团严重分歧；
3. $S_{\mathrm{indep}}<0.3$：来源高度依赖；
4. $p_{\mathrm{calibrated}}<0.35$ 且 $S_{\mathrm{suff}}<0.5$：校准置信度低且证据不足。

触发拒答后，系统输出“暂不能确认”“证据不足”或“建议人工复核”，同时记录触发条件、相关分项分数和证据摘要，不再强行给出确定性的真伪结论。

## 参数维护建议

为避免头文件和实现文件中出现相互矛盾的硬编码，建议将以下参数集中定义在统一配置中，并在验证集上重新估计后再调整：

| 参数 | 建议初始值 | 用途 |
| --- | ---: | --- |
| $\theta_\rho$ | 0.20 | 主题相关性过滤阈值 |
| $\lambda$ | 0.01/天 | 时效性衰减速率 |
| $\alpha,\beta$ | 4, 2 | 内容分数 sigmoid 参数 |
| $\gamma$ | 0.50 | 证据充分性缩放参数 |
| $\theta_{\mathrm{suff}}$ | 0.30 | 信息不足阈值 |
| $\lambda_1,\lambda_2,\lambda_3,\lambda_4$ | 0.35, 0.25, 0.20, 0.20 | 综合评分权重 |
| $\varepsilon$ | $10^{-8}$ | 防止除零和 logit 溢出 |

若修改公式、阈值或标签区间，应同步更新对应的头文件注释、实现代码、单元测试和本 README。
