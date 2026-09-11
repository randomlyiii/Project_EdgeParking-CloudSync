# 云端API调用测试（第7步 · P7-01）

本目录是**云端兜底接口的独立验证工具**：不依赖板子、不依赖 pip 安装任何包，
双击 `run_test.bat` 即用。它发出的请求与板上 Core1（`park_ui` 的
`CloudClient`）**逐字段一致**，解析响应用的是同一套三级容错解析逻辑，
所以这里跑绿 = 板端契约成立。

## 文件

| 文件 | 用途 |
|---|---|
| `run_test.bat` | **启动入口**（双击/命令行均可），自动找 Python、选模式、跑完暂停 |
| `cloud_api_test.py` | 测试主体（纯标准库：urllib + ssl + base64 + http.server） |
| `sample_key.txt` | **公开**（要上传 GitHub）：只放 `api_key="sk-xxx"` 占位，提供 `base_url`/`model` 参考 |
| `key.txt` | **私有**（已在 `.gitignore`）：真实 `api_key`，可带 `base_url`/`model` |
| `车牌.jpg` | 样例车牌图片（613x396），默认识别目标 |

## 密钥与隐私（重要）

本目录会传到 GitHub，因此**真实 Key 只写在 `key.txt`，而它被仓库 `.gitignore` 忽略**
（`git add --dry-run` 实测只会上传 `cloud_api_test.py` / `run_test.bat` / `README.txt` /
`sample_key.txt` / `车牌.jpg`，**不含 `key.txt`**）。

- 脚本**只从 `key.txt` 取 Key**；`sample_key.txt` 的 `sk-xxx` 被显式拒绝，永远不会当 Key 用。
- `key.txt` 支持三种写法：裸 `sk-...` 一行、`api_key="sk-..."` 片段、整段 `client = OpenAI(...)`。
- `base_url` 优先取 `key.txt` 的 `api_base`/`base_url`，没有才退回 `sample_key.txt` 的参考值；
  `model` 同理（`key.txt` → `sample_key.txt` → `--model` 覆盖）。
- 日志、`--json-out`、`--show-body` 里 **Key 一律只出现掩码**（`sk-7b8b...c901f`），
  离线自测里有一条专门的断言在守这个红线。

> 换 Key：只改 `key.txt`。要提交代码时**别把 `key.txt` 加进 git**（若误加，先
> `git rm --cached key.txt`，并且**该 Key 应立即在控制台作废重发**——推到公开仓库即视为泄露）。

> 无 Python 时：装 Python 3.8+ 即可，**不需要 `pip install`**。
> 本机实测解释器：`%LOCALAPPDATA%\Programs\Python\Python314\python.exe`（无 pip、无第三方包）。

## 用法

```
run_test.bat            交互菜单（推荐）
run_test.bat 1          识别车牌图片（真实联网调用）
run_test.bat 2          连通性测试（纯文本，最便宜的一次往返）
run_test.bat 3          离线自测（77 项，不联网）
run_test.bat 4          图片信息（格式/尺寸，不联网）
run_test.bat 5          批量探测模型名是否可用
run_test.bat 6          打开本说明
run_test.bat 7 / 8      用 qwen-vl-plus / qwen-vl-ocr 识别
```

其它参数直接透传给 python，例如：

```
run_test.bat --mode recognize --image D:\x\other.jpg --model qwen-vl-plus
run_test.bat --mode recognize --show-body --json-out result.json --raw raw.json
run_test.bat --mode test --base https://api.deepseek.com/chat/completions --model deepseek-chat
run_test.bat --help
```

退出码：`0` accepted（识别到车牌）/ `1` failed（网络或接口失败）/
`2` unreadable（无车牌或置信度低）/ `3` 用法或环境问题。
`--mode selftest` 全过即 `0`；`--mode models` 只要有任意一个模型返回 200 即 `0`。

## 请求契约（与 `docs/protocols.md` §5.1 一致）

```
POST <base_url>
Content-Type: application/json
Authorization: Bearer <api_key>

{"model":"qwen3-vl-plus",
 "messages":[{"role":"user","content":[
   {"type":"text","text":"<只输出 JSON：{\"plate\":\"...\",\"confidence\":0.x}>"},
   {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<...>"}}]}],
 "max_tokens":64,"temperature":0}
```

响应解析三级容错（同 `cloud_client.cpp`）：
去 ```` ``` ```` 围栏 → **括号配对**取第一个平衡 JSON 对象 → `json.loads`；
`plate` 去掉模型自带的分隔符（`·`/`•`/`-`/`.`/空格，**Core0 白名单是逐字节
精确比对**，留着分隔符就匹配不上、开不了闸）→ 非空且 UTF-8 ≤15 字节
（对齐 shm `plate[16]`，超出截断）；`confidence` 夹取到 `[0,1]`。
分类：`>= accept_conf(0.50)` = accepted，否则 unreadable，非 200 / 解析失败 = failed。

## 模型名（重要）

这里是 **OpenAI 兼容的 `/chat/completions`**，只能用**对话/视觉理解**模型。
`wan2.x-t2v-*`（文生视频）、`wanx-*`（文生图）、`cosyvoice`/`paraformer`（语音）
等**其它家族的模型一律 404**：`Unsupported model ... for OpenAI compatibility mode`
（工具现在会直接把这句话翻译成"这是文生视频模型"）。

实测可用（本 Key，2026-09-11）：

| 模型 | 结果 | 耗时 | 说明 |
|---|---|---|---|
| `qwen3-vl-plus` | ✅ `川A88888` 0.98 | 1.2~2.6s | **推荐**（当前代） |
| `qwen3-vl-flash` | ✅ `川A·88888`→规范化 `川A88888` 0.95 | 0.8s | 最便宜 |
| `qwen-vl-max` | ✅ | 0.5s | 上一代，精度好 |
| `qwen-vl-plus` | ✅ | 0.4s | 上一代，便宜 |
| `qwen-vl-ocr` | ✅ `川A·88888`→`川A88888` 0.99 | 1.3s | OCR 专用 |
| `qwen-plus` / `qwen-turbo` | ✅ | ~0.5s | 纯文本，只做连通性验证 |
| `qwen3-vl-max` / `qwen2.5-vl-72b-instruct` | ❌ 404/403 | — | 名字不存在 / 未开通 |

用 `run_test.bat 5` 可以随时重新探测这张表。

## 实测结论（2026-09-11，本机）

- ✅ **端到端识别成功**：`--mode recognize` → `qwen3-vl-plus` →
  `{"plate":"川A88888","confidence":0.98}` → 分类 `accepted` →
  对应 shm 写入 `plate=川A88888 confidence=0.98 result_source=1 result_valid=1`，
  全程约 1.2~2.6s（板端 Core1 超时预算 5s，余量足够）。
- ✅ **网络与鉴权全通**：DNS/TCP 443/证书校验（Python 3.14 + OpenSSL 3.5.7，
  系统 CA 78 张）正常，请求 0.3~2.6s 拿到响应。
- ⚠️ **踩过的两个坑**：① 账号曾处于「仅使用免费额度」模式且额度用尽 →
  全模型 `403 AllocationQuota.FreeTierOnly`（关掉该模式/充值后即恢复，
  现已可用）；② `key.txt` 里 `model=` 被填成 `wan2.7-t2v-*`（文生视频）→
  必然 404，已改回 `qwen3-vl-plus`。
- ✅ **离线自测 83/83 通过**：请求体字段、HTTP 头、UTF-8、三级解析、
  分隔符规范化、越界夹取、UTF-8 截断边界、400/403 错误信封、连接失败分类、
  图片嗅探，以及**密钥来源与隐私规则**（占位 Key 被拒、真 Key 只从 `key.txt` 读、
  Key 不进结果文件、掩码隐藏原文）。

## 排错

| 现象 | 原因 / 处理 |
|---|---|
| `403 AllocationQuota.FreeTierOnly` | 免费额度用尽：关「仅使用免费额度」或充值（本 Key 已恢复） |
| `401` | Key 错误/失效（`key.txt`） |
| `404 Unsupported model ... for OpenAI compatibility mode` | **用了非对话模型**（`wan*` 文生视频 / `wanx` 文生图 / 语音），换 `qwen3-vl-plus` |
| `404 model_not_found` | 模型名不存在，用 `run_test.bat 5` 探测真实可用名 |
| `403 access_denied` | 模型未开通：百炼「模型广场」点开通 |
| `network: ...` | 网络/DNS/代理；企业网可能需要放行 `dashscope.aliyuncs.com:443` |
| 车牌显示成 `\u7ca4B12345` | 当前控制台编码装不下中文（GBK 管道），
  **`--json-out` 里是完整原文**；在 UTF-8 终端（`run_test.bat` 已 `chcp 65001`）显示正常 |
| 板子上 HTTPS 失败 | 板端 rootfs **没有 CA bundle**，见 `docs/protocols.md` §5.4：放 `/etc/park/ca.pem` 或临时 `insecure_tls=1` |

## ⚠️ 顺带发现：板端 `cloud_client.cpp` 缺同一处规范化

本工具（已修）会把 `川A·88888` 规范成 `川A88888`；而 `qwen3-vl-flash` 与
`qwen-vl-ocr` 实测**就是带 `·` 输出**的。板端 `cloud_client.cpp` 的
`parsePlateJson()` 目前只去掉空格/换行/引号（1010 行附近），**没有去掉 `·`**，
而 Core0 白名单是**逐字节精确比对**（`core0_service/business/whitelist.c`
的 `wl_match` → `strcmp`）⇒ 用这两个模型时板子会 `DENY (not in whitelist)`、
**开不了闸**。建议给板端补同一段规范化（一行 `for` 循环去掉
`· • - . 空格`），或在 `core0.conf` 白名单里照抄带 `·` 的写法。


日志与 `--json-out` 中 **Key 一律只出现掩码**（`sk-7b8b...c901f`），不落明文。
