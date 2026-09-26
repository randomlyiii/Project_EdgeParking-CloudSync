# Git 提交规范（Git_Update_standard）

> 本规范记录本仓库的提交纪律与拆分方法。纪律源头见 `AGENTS.md`（§五 git 备忘），此处展开执行细则。

## 一、总纪律（不可违背）

1. **⛔ 不自动提交**：日常改动只留工作区；**用户明确说"提交 / commit"才提交**。同理，push 也要等用户明确指示。
2. **🧩 一功能 = 一关注点 = 一次提交**：不同功能不许混进同一提交；同文件装了多个功能时用 `git add -p` 按 hunk 拆开。
3. **docs 独立隔离**：文档改动一律单独成 `docs(...)` 提交，不与代码混合。
4. **先方案后执行**：改动面跨多板/多文件时，先列提交拆分方案（板子/业务/文件/难度）给用户确认，再动手。
5. **提交前自查**：`git diff HEAD --stat` 按功能分组核对；确认无密钥、无真配置、无构建产物混入。

## 二、提交信息

- 格式：`类型(板子/模块): 中文摘要`，类型用 `feat / fix / docs / chore / refactor / test`（conventional commits 简化版，与仓库历史一致）。
- **中文不进命令行**：提交信息写到仓库外的临时文件，用 `git commit -F` 读入：
  ```powershell
  # PowerShell 示例（路径纯 ASCII）
  Set-Content -Path "$env:TEMP\parkmsg1.txt" -Value "feat(core1): 云端兜底 enabled 总开关`n`n- 配置项 + LCD 设置页勾选`n- 低置信/失败两条路径都门控"
  git commit -F "$env:TEMP\parkmsg1.txt"
  ```
- 提交信息写"做了什么、为什么"，不复述 diff 能看到的细节。

## 三、拆分维度（按优先级）

| 维度 | 规则 | 示例 |
|---|---|---|
| **板子** | 不同板的代码绝不混提 | `rk3588_service/`(RK) 与 `core1_ui/`(MP157) 分开 |
| **业务** | 一关注点一提交，哪怕同文件 | `k210_link` 的 tcp 模式 与 FPS 显示 拆两个提交 |
| **文件** | 纯新增目录可整体一提；修改文件按 hunk 拆 | 新服务整套 = 1 个 `feat(rk3588)` |
| **难度** | 高难度（跨文件协议/架构）单独成提，便于回滚定位 | tcp 中继模式不与顺手的小改同提 |

**hunk 拆分实操**：`git add -p <file>` 逐个 hunk 选 y/n；拆分前用 `git diff <file>` 确认各主题在文件里物理分离（本次 `k210_link.cpp` 的 tcp 块与 fps 块即如此）。文件内纠缠太深（如 `edge_hub.py` 同时含 V4L2+bind+ROI）时不要硬拆，整文件归一个提交并在信息里列全关注点。

## 四、docs 提交的边界

- `docs(...)`：协议（先改 `PhaseMd/10` 母本 → 同步 `docs/protocols.md` → 变更记录）、README/Task、板级调试记录、验收文档。
- 协议类改动**必须与配套代码提交分开**（母本先行，代码随后，各自独立成提）。
- 调试记录类文档若混入他人未提交内容（如用户自己写的章节），按 hunk 拆成两个提交归属各自作者。

## 五、不入库清单（提交前核对）

- **AGENTS.md**（纯本地记忆，已 gitignore）；`docs/AGENTS_history.md` 是档案、可入库但归用户自己处置。
- **真配置/密钥**：只提交 `sample_*` 模板（纯 ASCII + 占位值），真文件（cloud.conf/wpa_supplicant.conf/park-ui.env/core0.conf/key.txt）一律 gitignore。
- **板级系统配置**：netplan/NetworkManager/sshd/authorized_keys 等**直接改在板上**，不进仓库（装机业务单元如 `deploy/systemd/*.service` 除外）。
- **构建产物**：`Debug/`、`.exe`、`bin/` 等（见 .gitignore）；模型权重（`.rknn/.kmodel`）与固件二进制。
- **参考资料/截图**：`参考资料/` 个人存档，默认不入库。

## 六、提交后

- 汇报每个提交的哈希与一句话内容；`git push` 等用户发话。
- 板上部署与提交解耦：提交 ≠ 部署，部署另按各板 README 流程。

## 附：本次（2026-09-27 RK3588 接入）提交方案示例

10 个提交：7 代码（rk3588 服务全套 / core1 tcp 模式 / core1 FPS+15fps / core1 enabled 开关 / 门禁 / deploy 链路 / k210 取景框）+ 3 docs（协议 §6+母本 / README / 调试记录 §八§九），hunk 拆分点 = `k210_link.*` 与 `main.cpp`。
