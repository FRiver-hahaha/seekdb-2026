# 赛题四：全文索引调优 — 瓶颈定位、修改与性能对比

分支：`fts-tuning`，提交：`4d0646361 perf(fts): share built IK dictionaries across documents`
（另含本次新增的词典区间顺序修复与回归用例）

测量环境：本机（15 GB RAM / NVMe，seekdb RelWithDebInfo，`tools/benchmark/fts_large_bench.sh`
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30）。

---

## 1. 瓶颈定位依据

不是靠读代码推断，而是用三组直接测量把 build / tokenize / query 的成本拆开。

### 1.1 本机绝对值不等于 CI，只能用同机前后对比

| 指标 | CI 基线 | 本机 before | 倍数 |
|---|---|---|---|
| `select1_avg_ms` | 0.210 | 0.099 | 2.1× |
| `tokenize_ik_avg_ms` | 0.765 | 0.184 | 4.2× |
| `query_cn_avg_ms` | 16.66 | 1.92 | 8.7× |
| `build_ik_all_sec` | 35.28 | 4.87 | 7.2× |

本机比 CI runner 快，而且各类指标快的倍数差得很大（2.1×–11×）。因此本机数据无法换算成
赛题分数，只有「同一台机器、同一份数据、改前改后相对值」有解释力。

### 1.2 同机噪声：build 4–10%，query 15–20%

在**未修改的二进制**上重复执行建索引与查询：`build_ik_all` 4.954–5.159 s，
`build_ik_content` 3.849–4.255 s。后续交错 A/B 进一步测出 query 侧同一 arm 自己轮对轮的
散布就有 **15–20%**（`query_cn` 2.01 → 1.92 → 1.94 → 2.22 ms），比 build 的噪声还大一个量级。
也就是说单次跑分得到的、以及「各跑一次的前后差值」在 15% 以内都不能作为结论；这条约束直接
决定了本报告的表述方式（见 §3）。

### 1.3 分词成本结构：固定成本在词典，不在逐字符

扫描文本长度并拟合「固定成本 + 每字符成本」：

- 每个 parser（space / ngram / ngram2 / beng / ik）都有一份约 **0.020 ms/文档** 的公共
  固定成本，来自 `tokenize()` 表达式层（每次重建并重新解析 JSON parser 属性、helper 重新
  init、每次新建词频 hash map），不是 parser 构造。
- IK 在此之外**每文档额外付约 0.023 ms 的词典构建成本**。定位依据：一个 IK parser 在
  `init_dict()` 里对 main/quantifier/stopword 各做一次 bucket 锁查找，缓存未命中时按
  `DEFAULT_KEY_PER_RANGE = 50000` 把 275,908 个编译期词典词重建成双数组 trie 区间。
  词典是编译期常量，这份成本每次重建结果完全相同。
- IK 每字符约 **0.47 µs**，而且是访存受限而不是指令受限：不构成词典词的稀有汉字与常用
  汉字每字符耗时相同（0.49 vs 0.47 µs）。所以时间花在 MAX_WORD/SMART 在每个位置上对
  双数组 trie 的随机访问上——这解释了为什么逐字符的微优化（每字符只解码一次、把已算好的
  字符直接交给 segmenter、区间二分）实测**毫无变化**。
- 把 `tokenize_ik` 的 0.184 ms 拆开看：约 0.099 ms 是 `SELECT 1` 同法测出的 SQL/协议下限，
  约 42 µs 是逐字符（压测文本约 90 个汉字 × 0.47 µs），约 43 µs 是每文档固定成本，
  三项相加正好闭合到 0.184 ms。也就是说分词指标本身有一半被不可优化的 SQL 往返稀释，
  任何分词侧改动的百分比都会被腰斩。

### 1.4 查询成本：没有大的固定开销，成本随命中数线性增长

- 只命中 0 行的 `MATCH` 查询仅比 `SELECT COUNT(*)` 下限高约 0.18 ms。
- 用 0 命中查询扫描词数，得到每词固定成本约 **0.048 ms/词**（1/2/4/8 词：0.305 / 0.370 /
  0.410 / 0.641 ms，下限 0.197 ms）。
- 真实查询成本随命中文档数近似线性，约 **0.21–0.31 µs/命中**（`query_cn` 8001 命中 1.92 ms，
  `query_beng` 11000 命中 2.84 ms）。
- 例外：任何需要回表返回真实行的查询固定花约 2.5–2.8 ms，返回 1 行和返回 8000 行一样，
  `LIMIT` 不会短路——这正是 `query_limit` 的来源。

结论：查询侧的大头在**每条命中文档上的重排评分与合并**，而不是扫描建立。`EXPLAIN EXTENDED`
证实了这点，即使 SQL 只有 `COUNT(*)`：

```
1 - output(nil), filter(nil), rowset=256
    calc_relevance=true, match_expr(MATCH(docs.title, docs.content) AGAINST('全文索引 分词器')),
    pushdown_match_filter(BOOL(MATCH(...) AGAINST('全文索引 分词器')))
```

`need_calc_relevance` 在 `ob_log_plan.cpp:12233` 被硬编码为 `true`，唯一能把它置 false 的
`ObTransformUtils::check_need_calc_match_score` 调用在 `ob_log_plan.cpp:12301-12311` 被整段
注释掉（其公有入口在本版本中已不存在）。后果是 BM25 对每条命中文档求值，而结果只被当成
「是否命中」的布尔值使用，同时每条查询还会为每个词创建、reuse、rescan 一个
`inv_idx_agg` DAS 扫描迭代器，而这个迭代器在文本检索的 DAAT 路径里**从未被读取过**
（`ob_text_retrieval_token_iter.cpp` 只用 `estimate_row_count()` 取词频文档数）。

---

## 2. 主要修改及其影响

### 2.1 已提交：共享已构建的 IK 词典（`4d0646361`）

把「构建好的区间词典」从 parser 移到 `ObFTDictHub`：每种词典类型全局只构建一次，之后只读
共享。

- `src/storage/fts/dict/ob_ft_dict_hub.{h,cpp}`：新增 `shared_dict_map_`、自有 arena
  `dict_alloc_`（标签 `FTDictShared`）和 `build_lock_`。`get_shared_dict()` 用双检锁发布；
  刻意**不**在 `rw_dict_lock_` 下调用 `load_cache()/build_cache()`，因为后者自己会取 bucket
  锁，嵌套会死锁。
- `src/storage/fts/ob_ik_ft_parser.{h,cpp}`：`init_dict()` 缩成三次 hub 查找；`reset()`
  只置空指针，不再析构词典。

影响（实测的固定成本下降 + 可检验的预期）：IK 每文档固定成本 0.043 ms → 0.030 ms，
即省下 13 µs/文档（剩下的 30 µs 是 `tokenize()` 表达式层与词典查找本身的锁 + hash，
并没有被这次改动消除）。折算到构建：本机 `build_ik_all` 每文档 243 µs，13 µs ≈ **预期 −5.3%**；
CI 上每文档 1.76 ms，按 §1.1 的分词倍数 4.2× 折算省下约 55 µs ≈ **预期 −3.1%**。
共享词典常驻内存实测为 RSS 271 MB（15 GB 机器），不构成内存压力；`ObFTCacheRangeHandle`
内嵌 `ObKVCacheHandle`，容器与词典同生命周期即意味着词典区间被持续 pin，不会被 KV cache
淘汰后留下悬垂的 `dat_block_` 指针。

### 2.2 已提交：逐字符路径简化（明确标注为清理，非性能收益）

- `src/storage/fts/ik/ob_ik_char_util.h`：`do_classify()` 改为解码一次后判断各字符类别，
  原来 `is_alpha/is_arabic/is_chinese/is_other_cjk/is_surrogate_high/is_surrogate_low` 会对
  同一段字节最多重复解码 6 次。与基线逐行等价：判定顺序、短路位置、解码失败的错误码传播
  路径完全一致；非 UTF-16 字符集下代理对判定原本恒为 `false`，用
  `constexpr bool check_surrogate` 保留，避免引入语义变化。
- `src/storage/fts/ik/ob_ik_processor.{h,cpp}`：删掉 `ObIIKProcessor::process()` 这层包装，
  调用方把已经算好的 `(ch, char_len, type)` 直接传给 `do_process()`，不再让每个 segmenter
  通过两次虚调用重新取一遍。
- `src/storage/fts/ob_ik_ft_parser.*`：segmenter 从 `ObList` 改为定长数组
  （`SEGMENTER_CNT = 4`），并保持 letter→quantifier→CJK→surrogate 的原顺序（顺序会影响
  上下文状态，不能换）。

按 §1.3，这些改动实测为**零变化**，保留理由是去掉重复解码与重复虚调用属于确定性简化。

### 2.3 已提交：词典区间查找改为二分

`src/storage/fts/dict/ob_ft_range_dict.cpp:find_first_char_range()` 原本是线性扫描每个区间、
每个区间两次 `ObCharset::strcmp`，对每个 CJK 字符都要走一遍。改为二分查找，比较原语仍是
`ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN)`，因此 PAD SPACE / 排序规则语义不变。

### 2.4 本次新增：把二分依赖的顺序条件变成显式条件

`find_first_char_range()` 的正确性依赖 `range_dicts_` 按 `start_` 升序且互不重叠。我用脚本
按 `build_ranges_concurrently_thread_pool()` 的切分规则重放了真实的编译期词典
（`src/storage/fts/dict/ob_ik_dic.cpp`）：

| 词典 | 词数 | 区间数 | 相邻逆序 | 区间单调不重叠 | 552k 次探测线性/二分不一致 |
|---|---|---|---|---|---|
| `main_dic` | 275,908 | 6 | 0 | 是 | **0** |
| `quan_dict` | 316 | 1 | **2** | 是 | 0 |
| `stop_dic` | 33 | 1 | 0 | 是 | 0 |

结论：对当前词典二分与线性**逐点等价**；但 `quan_dict` 里已经存在 2 处词序逆序，说明「生成
的词典数组是有序的」只是巧合而非约束。一旦某个词典的区间数超过 1，顺序假设就会静默失效。
因此把区间顺序在构建时显式建立：`build_dict_from_cache()` 收集完区间后按 `start_` 做一次
插入排序（区间数量是 `词数/50000` 量级，个位数，成本可忽略）。这样二分的正确性前提来自代码
自身，而不是来自外部数据恰好有序。

验证脚本：`verify_range_binary_search.py`（本地留存，不入仓库）。

---

## 3. 优化前后性能对比

### 3.1 方法：交错只读 A/B + 一个空对照指标

两个二进制交替、同机同数据、每轮内配对，共 4 轮（偶数轮反过来先测 after，抵消位置效应）：

- `seekdb_step0_baseline`：未含任何本次改动的内核（用 `FTDictShared` 标签计数 0 验证身份）。
- `seekdb_step2_range_order`：本次全部改动（共享词典 + 逐字符简化 + 区间二分 + 区间排序）。
- 只用 `tools/benchmark/fts_large_bench.sh SKIP_LOAD=1`：不建库、不灌数、不 drop/create 索引。
  benchmark 脚本、SQL、计时逻辑、评分脚本、基线文件全部未改动。
- 每轮另外校验 4 个查询的命中数，两侧均为 8001 / 11000 / 7332 / 20，与 CI 基线一致。

关键的是把 **`select1_avg_ms` 当空对照**：它是一条与全文检索完全无关的 SQL，本次改动不可能
影响它，所以它在两个 arm 之间的差值就是「本机在当前状态下能凭空测出来的系统偏差」。

| 指标 | base 四轮 | after 四轮 | 配对均值 | arm 内轮间散布 |
|---|---|---|---|---|
| `select1_avg_ms`（空对照） | 0.1025 0.0965 0.1022 0.0995 | 0.0983 0.0978 0.0982 0.0989 | **+1.82%** | 5.9% / 1.1% |
| `tokenize_ik_avg_ms` | 0.1800 0.1803 0.1797 0.1828 | 0.1698 0.1741 0.1774 0.1734 | **+3.88%** | 1.7% / 4.4% |
| `tokenize_beng_avg_ms` | 0.1311 0.1313 0.1331 0.1334 | 0.1299 0.1287 0.1322 0.1316 | +1.23% | 1.7% / 2.7% |
| `query_cn_avg_ms` | 2.0133 1.9206 1.9428 2.2202 | 1.8774 1.9289 2.2232 2.2022 | −1.83% | 15.1% / 16.7% |
| `query_beng_avg_ms` | 2.7366 2.7484 2.7010 3.2039 | 2.7414 2.7427 3.2357 3.2778 | −5.52% | 18.3% / 17.9% |
| `query_mixed_avg_ms` | 2.0313 2.0668 2.0489 2.3720 | 2.0399 2.1004 2.4874 2.4749 | −6.95% | 16.6% / 19.6% |
| `query_limit_avg_ms` | 2.5748 2.4983 2.3883 2.3550 | 2.4566 2.4301 2.5911 2.4115 | −0.89% | 9.0% / 7.4% |

### 3.2 这些数字说明什么

- **`tokenize_ik` 是唯一能判定为真实收益的指标。** 4 轮全部为正（+5.7% / +3.4% / +1.3% /
  +5.1%），arm 内散布只有 1.7–4.4%，配对均值 +3.88% 超过了空对照的 +1.82% 系统偏差，
  扣除后约 **+2%**。量级与 §1.3 独立测得的「IK 每文档固定成本 0.043 → 0.030 ms」吻合
  （13 µs / 180 µs ≈ 7%，再被约一半的 SQL 往返稀释后正好落到 3–4%）。
- **`tokenize_beng` 没有变化**，+1.23% 小于空对照的 +1.82%。这正是成本模型预测的结果：
  BENG 不构建 IK 词典，词典共享改动本来就不该影响它。指标与机理相互自洽，说明 tokenize 的
  收益不是碰出来的。
- **query 四个指标在本机无法判定。** 轮对轮的机器状态漂移达 15–20%（第 3、4 轮两个 arm 一起
  从 1.9 s/2.0 s 跳到 2.2 s/3.2 s），远大于本次改动的预期效应，配对后正负号在 −14% 到 +7%
  之间乱跳。机理上也只应有每查询一次的常数级改善（约 1% 量级），本机分辨率不足以证实或否定。
- **build 三个指标没有测量。** 计时建索引必须反复 drop/create 索引，会改动已准备好的压测表，
  未在没有确认的情况下执行。按 §2.1 的成本折算，预期约 −5%（本机）/ −3%（CI），同样落在
  本机 4–10% 的噪声带里。

按可测部分折算（tokenize +2.56% 原始值 / 扣对照后约 +0.7%，query −3.80% 但不可判定），
**本机不能证明本次改动带来正的赛题得分**；如果 build 侧拿到预期的 3–5%，综合约 +1–2%，
对应赛题分数仍然接近 0 分。这是一个诚实但不好看的结论，其根因是本机对 ±5% 量级的改动
没有分辨率（§1.2、§3.2）。

### 3.3 反例：为什么单次改前/改后跑分必须丢掉

最早的一对单次完整跑分（含建索引）给出的是：

`build` −3.79%、`tokenize` +3.28%、`query` **−10.14%**、综合 −3.55%。

交错配对之后，query 的 −10%~−15%「回退」不复现（同一 arm 自己轮间就能漂 15–20%），
说明那是机器漂移而不是改动后果。这一对旧数据保留在这里只作为方法论反证：**同一台机器上
各跑一次的前后对比，在这个噪声量级下不足以支撑任何结论。**

---

## 4. 正确性验证

性能分数以正确性为前提，所以这里的验证不依赖 benchmark 命中数。

1. **仓库既有回归用例**：`tools/deploy/mysql_test/test_suite/fts_index/` 全部用例，在含本次
   全部改动的二进制上 **14 通过 / 2 失败**；未修改二进制上的基线是 13 通过 / 2 失败（第 14 个
   就是下面新增的用例），两个失败用例完全相同且与全文检索无关：
   `basic_dml` 失败在一条内部表查询的语法（`near 'such row'`），`partitioned_simple_query`
   只差 EXPLAIN 里的占位符编号（`:0` vs `:1`）。其中 `cn_word` 是用例集里原本唯一会执行
   `alter system flush kvcache cache='dict_cache'` 的用例，直接关系到本次词典生命周期改动；
   `tokenize_function`、`parser_properties`、`create_fts_index_afterward`、
   `offline_ddl_with_fts_index`、`online_ddl_on_table_with_fts_index` 也全部通过。
2. **针对本次改动点新增用例**：
   `tools/deploy/mysql_test/test_suite/fts_index/t/fts_dict_share_and_char_class.test`
   （+ 对应 `.result`），三段分别钉住 §2.2 / §2.1 / §2.3-2.4：
   - 角色分类：拉丁、阿拉伯、假名、谚文、半/全角、CJK 基本区与扩展区边界字符
     （`一龻鿿㐀`）、数字与连接符（`1.2亿`、`all-in-one`、`email@example.com`）、
     以及同一批文本走 beng 的结果，全部是逐 token 的精确输出。
   - 词典共享：同一张表上同时建 smart / smart / max_word 三个 IK 索引（两种模式的词典在 hub
     中并存），插入按字符类别交替的多行以捕捉跨文档残留状态，然后
     `flush kvcache cache='dict_cache'`，再验证：重建 parser 的 tokenize 输出、
     四个 `MATCH` 计数、flush 之后新写入文档的分词结果、以及 drop/re-create 索引后的计数
     全部与 flush 前一致。
   - parser 隔离：beng / ngram2 / space / ik 在同一张表上的精确输出与命中数。
   手工另跑 3 次结果完全一致（`tokenize()` 输出顺序来自词频 map 的桶序，是确定性的）。

## 5. 顺带发现的两个既有缺陷（本次未修）

1. `tokenize(<TEXT 列>, 'ik')` 以及长度约 ≥1000 字符的 IK 输入返回
   `ERROR 1210 (Arguments to ... )`，在**未修改**的服务端即可复现；`beng` 不受影响。
   这是一个独立的功能缺陷，修它超出了性能调优的范围。
2. `src/storage/fts/dict/ob_ft_cache.h:47-61`：`ObDictCacheKey::hash()` 把 `range_id_` 计入
   （第 59 行），而 `operator==`（第 50-51 行）只比较 `name_` 与 `dict_type_`。哈希与相等判定
   不一致：同一词典不同区间的 key 哈希不同但互相判等，只有落进同一个桶链时才会返回错误区间的
   value，所以是偶发的隐患。它是本次改动依赖的既有代码，但不是本次改动引入的，修它会改变缓存
   命中行为、可能牵连性能分数，因此只报告不夹带。

## 6. 已识别但没有实施的改动，以及原因

### 6.1 构建侧：每文档新建/销毁词频 hash map

`ObDASDomainUtils::generate_fulltext_word_rows()`（`src/sql/das/ob_das_domain_utils.cpp:290-297`）
和 `tokenize()`（`src/sql/engine/expr/ob_expr_tokenize.cpp:87-94`）都为**每一个文档**新建一个
`ObFTWordMap`（`ObHashMap`）：`cal_next_prime()` 取桶数、分配并清零桶数组、每个唯一词一次
节点分配、结束时再遍历全部桶释放。它的持有者（`ObFTDMLIterator::ft_parse_helper_`、
`ObFTIndexRowCache`）本来就跨很多文档存活，所以把 map 提到持有者上、逐文档 `reuse()`
是安全的，也正是赛题说的「复用缓冲区、减少内存分配」。

没有实施的原因是**它无法被本机测量证明**：每唯一词的成本与「每唯一词必须产生一条索引行 /
一个 JSON 数组元素」这些不可消除的工作纠缠在一起，用 SQL 差分（扫词数/唯一词数拟合）分不出
只属于 map 分配的那一份。按结构估算 map 约 3–4 µs/文档，即 tokenize 的 4–5%、build 的 1–2%，
与已提交改动同一量级，但低于本机 4–10% 的噪声。在已经有一项未被本机证明的改动的情况下，
再叠一层同样的改动只会让归因更差，所以留给有 CI 数据的环境验证。

### 6.2 查询侧：相关性裁剪

见 §1.4 与上文，这是查询类别唯一的大杠杆，但它等于重建被上游主动关掉的优化器逻辑，风险高于
收益，未实施。
