# 重明鸟算法层输入 JSON 结构（审核稿）

## 1. 数据所处位置

本结构不是工作 agent 的原始输出，而是**预处理 agent 完成字段补齐、同义词归一和枚举标准化后**，交给 `news_qualifier` 的最终输入。

```text
新闻/事件
  └─ 工作 agents：拆分、检索、NLI、关系维度评分、陪审团评审
       └─ 预处理 agent：补齐字段 + 同义词归一 + 输出 schema_version=0
            └─ JsonReader：类型与业务约束校验
                 └─ 七层算法
```

`JsonReader` 不进行 NLP，不猜测同义词，也不补造缺失分数。模型权重、时间衰减系数、温度参数、错误预算和由独立校准集计算的 conformal 分位点不放入本 JSON。

## 2. 总体结构图

```text
QualifierRequest
├─ schema_version                 固定为 0
├─ request_id                     单次请求 ID
├─ event_id                       事件 ID
├─ original_text                  原始新闻文本
├─ source                         来源媒体
├─ publish_date                   发布日期
├─ event_nature                   事件性质
├─ domain                         领域
├─ event_time                     事件发生时间
├─ seven_elements                 事件级七要素（七个键必须齐全）
├─ atomic_claims[2..5]
│  ├─ claim_id
│  ├─ claim_text                  去语境化后的完整陈述句（算法新增必需字段）
│  ├─ seven_elements              声明级七要素
│  ├─ check_question              是/否核查问题
│  ├─ claim_type                  数值型/时间型/身份型/地点型/存在型
│  ├─ entities[]
│  ├─ canon_refs[]
│  ├─ keywords[]                  声明关键词
│  ├─ event_day                   整日序号或 null
│  ├─ time_sensitive              是否时间敏感
│  └─ evidence[]
│     ├─ evidence_id
│     ├─ original_source_id       同源转载使用相同 ID；未知可用空字符串
│     ├─ source_type              标准来源类型或 null
│     ├─ keywords[]               证据关键词
│     ├─ event_day                整日序号或 null
│     ├─ nli_label                support/refute/neutral
│     └─ nli_confidence           [0,1]
├─ logic_edges[1..4]
│  ├─ edge_id
│  ├─ claim_i / claim_j           引用 claim_id
│  ├─ rel                         temporal/causal/conditional/contrast/coordinate
│  ├─ required_dimensions[]       必须与 rel 的规范完全一致
│  ├─ dimension_scores{}          对应维度分数，均为 [0,1]
│  └─ nli_score                   关系成立概率，[0,1]
└─ jury_opinions[]                每条声明恰好一个分组
   ├─ claim_id
   └─ opinions[]                  无有效意见时使用空数组
      ├─ verdict                  support/refute/uncertain
      ├─ hybrid_retrieval_score   [0,1]
      ├─ confidence               [0,1]
      └─ completed_without_error  boolean
```

## 3. 逻辑关系与维度的固定对应

| `rel` | `required_dimensions` 与 `dimension_scores` 必须包含的键 |
|---|---|
| `temporal` | `S_time` |
| `causal` | `S_time`, `S_mech`, `S_counter`, `S_confound`, `S_dose` |
| `conditional` | `S_mech`, `S_counter` |
| `contrast` | `S_contrast` |
| `coordinate` | `S_coordinate` |

不允许缺少、重复或额外维度。`required_dimensions` 用于保留拆分 agent 的任务描述，`dimension_scores` 保存后续工作 agent 的实际评分。

## 4. 标准枚举值

`source_type` 只接受：

```text
official_registry, academic, open_web, social_media,
news_media, video_or_github, market_data
```

`nli_label` 只接受 `support`、`refute`、`neutral`；`verdict` 只接受 `support`、`refute`、`uncertain`。大小写或同义表达必须由预处理 agent 在进入算法前转换。

拆分规范虽然在类型定义中列出“因果型”，但同时明确要求因果命题拆成两条原子声明和一条 `causal` 边。因此算法输入会拒绝直接标为“因果型”的原子声明。

## 5. 可直接用于联调的完整样例

```json
{
  "schema_version": 0,
  "request_id": "REQ-001",
  "event_id": "EV001",
  "original_text": "2026年9月，因某州政府削减教育预算，导致当地学校关闭。",
  "source": "示例新闻",
  "publish_date": "2026-09-27",
  "event_nature": "待核查",
  "domain": "社会民生与文体教育",
  "event_time": "2026-09",
  "seven_elements": {
    "主体": "某州政府",
    "行为": "削减教育预算",
    "客体": "教育预算",
    "时间": "2026年9月",
    "地点": null,
    "数量": null,
    "结果": "学校关闭"
  },
  "atomic_claims": [
    {
      "claim_id": "C001",
      "claim_text": "某州政府在2026年9月削减了教育预算。",
      "seven_elements": {
        "主体": "某州政府",
        "行为": "削减",
        "客体": "教育预算",
        "时间": "2026年9月",
        "地点": null,
        "数量": null,
        "结果": "教育预算减少"
      },
      "check_question": "某州政府是否在2026年9月削减了教育预算？",
      "claim_type": "存在型",
      "entities": ["某州政府", "教育预算"],
      "canon_refs": [],
      "keywords": ["政府", "教育预算", "削减"],
      "event_day": 20715,
      "time_sensitive": true,
      "evidence": [
        {
          "evidence_id": "D001",
          "original_source_id": "ORIGIN-001",
          "source_type": "official_registry",
          "keywords": ["政府", "教育预算", "削减"],
          "event_day": 20715,
          "nli_label": "support",
          "nli_confidence": 0.91
        }
      ]
    },
    {
      "claim_id": "C002",
      "claim_text": "当地学校在预算削减后关闭。",
      "seven_elements": {
        "主体": "当地学校",
        "行为": "关闭",
        "客体": null,
        "时间": "预算削减后",
        "地点": null,
        "数量": null,
        "结果": "学校关闭"
      },
      "check_question": "当地学校是否在预算削减后关闭？",
      "claim_type": "存在型",
      "entities": ["当地学校"],
      "canon_refs": [],
      "keywords": ["学校", "关闭"],
      "event_day": null,
      "time_sensitive": false,
      "evidence": []
    }
  ],
  "logic_edges": [
    {
      "edge_id": "E001",
      "claim_i": "C001",
      "claim_j": "C002",
      "rel": "causal",
      "required_dimensions": ["S_time", "S_mech", "S_counter", "S_confound", "S_dose"],
      "dimension_scores": {
        "S_time": 0.88,
        "S_mech": 0.72,
        "S_counter": 0.61,
        "S_confound": 0.55,
        "S_dose": 0.40
      },
      "nli_score": 0.81
    }
  ],
  "jury_opinions": [
    {
      "claim_id": "C001",
      "opinions": [
        {
          "verdict": "support",
          "hybrid_retrieval_score": 0.86,
          "confidence": 0.90,
          "completed_without_error": true
        }
      ]
    },
    {
      "claim_id": "C002",
      "opinions": []
    }
  ]
}
```

## 6. 七层输入边界

| 层 | 从外部 agent 读取的字段 | 不从 JSON 读取的内容 |
|---|---|---|
| 1 证据真实性 | `source_type`、两侧 `keywords`、两侧 `event_day`、`time_sensitive`、`nli_label`、`nli_confidence` | 来源基线、时间衰减等配置 |
| 2 证据充分性 | `original_source_id` 形成来源簇；第一层结果 | 充分性函数参数 |
| 3 逻辑关系 | `rel`、`nli_score`、`required_dimensions`、`dimension_scores` | 关系组合权重 |
| 4 陪审团 | `jury_opinions` 中的四项意见字段 | 陪审团融合公式参数 |
| 5 多维综合 | 第一至四层计算结果 | `lambda1`–`lambda5` 五项聚合权重 |
| 6 概率校准 | 第五层 `MC_i`、充分性、内容方向和逻辑可用性 | fit 阶段冻结的温度、数值稳定常数和报告分箱数 |
| 7 拒答判定 | 第六层四类校准概率 | 用户预先指定的错误预算、独立 calib 按 `family_id` 计算的分位点 |

因此，第五至第七层不需要工作 agent 再提供一套原始数据字段；它们消费前面各层的计算结果和算法自身配置。第五层采用五项权重的逐声明公式，第六层输出四类温度校准概率，第七层以 conformal 预测集是否为单例作为唯一拒答入口。
