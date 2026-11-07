# 怎么加一个新动作

Pulse 的菜单、快捷键、命令面板（`>`）、Pulse 助手和外部 AI 应用（MCP）用的是同一张动作表。新功能在表里登记一次，这五个地方就都能用。漏了哪一步，`tools/actions/check_actions.py --strict` 会报出来；CI（`.github/workflows/l10n.yml`）用它把关，不通过就失败。

本文写给给 Pulse 加功能的人。外部 AI 应用怎么调用这些动作，见 [mcp.md](mcp.md)；用户怎么安装和打开 MCP，见 [../ai-pack.md](../ai-pack.md)。

## 一个动作要写在哪里

所有文件都在 `src/app/actions/` 下，另有说明的除外。

| 位置 | 写什么 | 什么时候必需 |
| --- | --- | --- |
| `action_inventory.h` 的 `kActions` | id、风险类、`by_target` | 总是 |
| `action_inventory.h` 的 `kKeyActionLinks` / `kMenuCmdLinks` | 把快捷键或菜单命令连到动作；不连的写原因 | 有快捷键或菜单命令时 |
| `action_registry.cpp` 的 `kDefs` | 标题、分组、图标、默认快捷键、菜单命令或 `run`、`fill` | 总是（实在没有界面行的，写进 `kNoRow` 并说明原因） |
| `action_args.cpp` 的 `kRunners`，或 `action_args_ext.cpp` 的 `kExtRunners` | 带参数执行，返回 `ArgRun`（结果、撤销、给模型的数据） | 动作有参数时 |
| `action_params.cpp` 的 `kSpecs` | 参数说明，模型和 `find_actions` 看的就是它 | 有执行函数时 |
| `action_args_ext.cpp` 的 `kAskIds` + `AskFor` | 确认卡的文字 | 风险类为 External 时 |
| `action_args_ext.cpp` 的 `kLockIds` + 锁定说明 | 为什么只能你自己做、在哪里做 | 风险类为 UserOnly 时 |
| `action_files.cpp` 的 `FileOpOf` | 计划卡（批量操作的同一张卡） | 风险类为 Move / Delete / PermanentDelete 时（另有流程的，写进 `check_actions.py` 的 `FLOW_MOVES`） |
| `tools/l10n/catalog.tsv` | 所有界面文字（简体、English、繁體） | 总是；改完运行 `gen_strings.py`，不要手改生成的文件 |
| `src/app/ai_assistant_agent.cpp` 中 `run_actions` 说明里的「Common actions」 | 常用动作和它的参数 | 可选；写进去的会被核对 |

快捷键的链接在编译期检查（`action_inventory.cpp`）；菜单命令、注册表、参数说明和确认文字由 `check_actions.py` 检查。

## 步骤

### 1. 起 id，定风险类

在 `kActions` 里合适的分组下加一行：

```cpp
{"tab.pin", ActionRisk::View},
```

- **id** 写成 `领域.动作`，只用小写字母和下划线（`tab.pin`、`file.new_folder`）。id 一旦发布就不要改：MCP 客户端靠它调用，「始终允许」的规则按 `pulse.<id>` 保存，改了就失效。
- **风险类**按这个动作最多会做什么来选，不按通常做什么。拿不准就选高一级。

| 风险类 | 含义 |
| --- | --- |
| `Read` | 只读状态 |
| `View` | 改变 Pulse 显示什么（栏、标签页、视图、选择），可以撤销 |
| `Organize` | Pulse 自己的数据（标签、收藏、托盘、别名），可以撤销 |
| `Create` | 只新建，不覆盖任何东西，可以撤销 |
| `Move` | 移动或重命名文件：用户确认一次计划 |
| `System` | 改动 Windows（开机启动、默认文件管理器、驱动器） |
| `External` | Pulse 无法预知结果（第三方菜单项、Git、其他程序）：每次确认，不能「始终允许」 |
| `Delete` | 移到回收站：每次确认 |
| `PermanentDelete` | 红色二次确认；系统文件夹一律不碰 |
| `UserOnly` | 需要用户本人（例如输入密码）：助手只告诉用户去哪里做 |

- **`by_target = true`**：目标会抬高风险。例如 `nav.open` 打开程序或脚本（`.exe .bat .cmd .ps1 .vbs .msi`、快捷方式）时按 External 处理。打开文档和媒体文件仍然直接执行。目标规则写在 `action_policy.cpp`。

动作对应已有的菜单命令或快捷键时，在 `kMenuCmdLinks` / `kKeyActionLinks` 里连上：

```cpp
{CmdTabPin, {"tab.pin"}},
```

不需要动作的命令也要写一行，并给出原因：

```cpp
{CmdSomething, {"", "why it has none"}},
```

### 2. 注册表行

在 `action_registry.cpp` 的 `kDefs` 里加一行。命令面板列出的、Pulse 助手的 `find_actions` 搜索的、MCP 的 `find_actions` 返回的，都是这一行。

```cpp
struct Def {
    std::string_view id;
    I title;               // 标题（catalog 里的字符串 id）
    I domain;              // 分组：I::ActDomTabs 等
    const wchar_t* glyph;  // Segoe Fluent Icons 字形
    P pictogram;           // 彩色图示，没有就 P::None
    const wchar_t* chord;  // 默认快捷键，没有就 kNoKey；显示的是用户当前的键
    int command;           // 要执行的 MenuCmd；用 run 时填 0
    Run run;               // 不走菜单命令时执行的函数；没有就 kNoRun
    Fill fill;             // 能不能用 / 为什么 / 当前值 / 选项；总能用就 kAlways
    Pick pick = nullptr;   // 有选项、又没有菜单命令时，按位置执行一个选项
    bool hidden = false;   // true：只给助手用，命令面板不列
};
```

- **能交给菜单命令就交给它**（`command`）：命令面板和助手执行时，和用户点菜单走同一段代码。
- **`fill` 要说真话**：不能用时给出原因，例如「现在只有一栏」。命令面板把这一行显示为灰色，在快捷键的位置写出原因，并排到后面；`find_actions` 返回 `enabled:false` 和这个原因，模型就不会去试。
- **需要用户自己选的动作**（打开系统菜单、自定义工具栏等）：`run` 只负责把已有的界面打开，由用户来选，不要替用户点。

### 3. 参数（可选）

动作需要参数时，写一个执行函数，并在 `kRunners` 里登记：

```cpp
ArgRun TabPin(AppState& s, const json::Value& args);
// ...
constexpr std::pair<std::string_view, Runner> kRunners[] = {
    {"tab.pin", TabPin},
```

`ArgRun` 的字段决定步骤卡和结果卡怎么显示，也决定模型拿到什么：

| 字段 | 用途 |
| --- | --- |
| `ok` / `error` / `why` | 是否成功；`error` 是给模型的英文原因，`why` 是步骤卡上的中文原因（用 `Fail(...)` 一次填好） |
| `skip` | 本来就是这样，什么都不用做。不算失败，也不进撤销 |
| `arg` / `sub` | 步骤卡上的小标签（例如「3 个」）和第二行 |
| `line_before` / `line_bold` / `line_after` | 结果卡上的那一行（用 `SetLine`） |
| `undo` | 撤销函数。**只在状态仍然是这个动作留下的样子时才恢复**；用户之后改过的，不要覆盖 |
| `read_only` | 只读的行：没有结果行，也不进撤销 |
| `data` | 给模型的额外字段 |

然后在 `action_params.cpp` 的 `kSpecs` 里写参数说明：用空格分隔，每项是 `名字:类型`。

```cpp
{"tab.pin", "tab:tab pin:bool"},
{"file.move_to", "items:paths dest:folder"},
{"view.sort", "by:name|modified|type|size|created|rating|duration desc:bool pane:pane"},
```

类型就是写给模型看的文字。常用的有：

| 类型 | 含义 |
| --- | --- |
| `string` | 字符串 |
| `bool` | true / false |
| `path` | 路径 |
| `folder` | 文件夹路径 |
| `paths` | 一组完整路径 |
| `pane` | 栏 |
| `tab` | 标签页；会自动展开成「编号、标题、all 或列表」的说明 |
| `setting` | 设置项 |
| `any` | 任意值 |
| `a\|b\|c` | 只能取这几个值 |

还有一个整体写法 `selector`，供 `select.items` 使用。

### 4. 风险类决定各个入口怎么做

同一个动作，不同的入口按同一套规则处理。助手的规则在 `action_policy.cpp` 的 `AiPolicy`，MCP 的分组在 `McpGroupOf`：

| 风险类 | 菜单 / 快捷键 / 命令面板 | Pulse 助手 | MCP（外部 AI 应用） |
| --- | --- | --- | --- |
| Read、View | 直接执行（是用户自己点的） | 直接执行；结果卡显示旧 → 新，整次调用一次撤销 | 「查看与界面」，默认开：直接执行 |
| Organize | 同上 | 同上 | 「整理 Pulse」，默认关；打开后直接执行，可以撤销 |
| Create | 同上 | 同上 | 「新建」，默认开：每次询问，可以选「始终允许这个文件夹」（含子文件夹） |
| Move、Delete、PermanentDelete | 同上（删除仍有原来的确认） | 先显示计划卡，用户确认后执行；永久删除要点两次红色按钮 | 「移动、重命名和删除」，默认关；打开后每次显示计划卡并询问 |
| System、External | 同上 | 每次先显示确认卡，不能「始终允许」 | 「Pulse 之外的效果」，默认关；打开后每次询问 |
| UserOnly | 同上 | 不执行，告诉用户为什么、去哪里做 | 「只能你自己做」：始终拒绝（`locked`） |

`settings.set` 是例外：助手直接执行，MCP 归入「整理 Pulse」；不能交给 AI 的设置（关闭删除确认、助手权限、始终允许、连接、API 密钥）由设置代码自己拒绝。

### 5. 确认文字、锁定说明、计划卡

- **External**：把 id 加进 `kAskIds`，在 `AskFor` 里写确认卡的标题、说明、对象和两个按钮（例如「不推送 / 推送」）。没有可做的事返回 `Skip`，做不了返回 `Fail`，这两种情况都不弹卡。
- **UserOnly**：把 id 加进 `kLockIds`，写锁定说明：为什么助手不做，用户自己去哪里做（例如「此电脑 → 双击 E 盘」）。
- **Move / Delete / PermanentDelete**：在 `FileOpOf` 里把动作映射到文件操作，计划卡、回收站容量检查和一次撤销都由 `action_files.cpp` 负责。不要自己弹对话框。

### 6. 文字和提示词

- 所有用户能看到的文字都写进 `tools/l10n/catalog.tsv`，然后运行 `python tools/l10n/gen_strings.py`。
- 常用的动作可以加进 `ai_assistant_agent.cpp` 中 `run_actions` 说明里的「Common actions」。不加，模型也能通过 `find_actions` 找到它。加了的话，`check_actions.py` 会核对写的参数都在 `kSpecs` 里。

### 7. 检查和测试

```powershell
python tools\l10n\gen_strings.py
python tools\actions\check_actions.py --strict
```

通过时输出一行统计，例如：

```
actions 134 · registry rows 132 · menu commands 142 (linked 139, excluded 3) · specs 60 · prompt actions 36
```

不通过时会逐条列出缺什么，例如：

```
tab.pin: no registry row (add it to kDefs, or to kNoRow with a reason)
```

再在 `src/app/selftest_1b2.cpp` 里补测试：

- 风险和分组：`TestActionPolicy`
- 参数和撤销：`TestActionArgs`
- `fill` 的状态：`TestActionState`
- MCP 的放行和预检：`TestMcpRunActions`

运行 `pulse.exe --selftest --test-instance`，结果要全部是 `[PASS]`。

## 完整例子：`tab.pin`（固定标签页）

下面是仓库里真实的写法，新动作照着写即可。

**1. 清单**（`action_inventory.h`）：只改变显示，所以是 View；菜单里的「固定」连到它。

```cpp
{"tab.pin", ActionRisk::View},
// ...
{CmdTabPin, {"tab.pin"}},
```

**2. 注册表行**（`action_registry.cpp`）：没有默认快捷键；不带参数时对当前标签页执行；`FillTabPin` 给出当前状态（已固定 / 未固定）。

```cpp
{"tab.pin", I::TabPin, I::ActDomTabs, L"\xE718", P::None, kNoKey, 0,
 +[](AppState& s) { RunTab(s, CmdTabPin); }, FillTabPin},
```

**3. 参数**（`action_params.cpp`）：可以一次固定几个标签页。

```cpp
{"tab.pin", "tab:tab pin:bool"},
```

**4. 执行函数**（`action_args.cpp`，节选）：找不到标签页时用 `Fail` 同时给模型和用户一个原因；已经是这个状态的标签页跳过；`undo` 只恢复仍然是它留下的样子的标签页。

```cpp
ArgRun TabPin(AppState& s, const json::Value& args) {
    std::vector<size_t> idx;
    if (!TabsForArg(s, args, idx)) return Fail("no such tab (get_state lists them)", I::AiActWhyNoTab);
    const bool pin = args.get_bool("pin", true);
    ArgRun r;
    r.arg = TabsLabel(s, idx);
    r.sub = NameList(TabNames(s, idx));
    // ... 只改需要改的，记下原来的位置和颜色 ...
    r.ok = true;
    if (changed.empty()) {
        r.skip = true;   // 本来就是这样
        return r;
    }
    // ... 执行 CmdTabPin ...
    SetLine(r, Fill(Text(pin ? I::AiActLinePinned : I::AiActLineUnpinned), L"{tabs}", what), what);
    r.undo = /* 仍是这个动作留下的样子才恢复 */;
    return r;
}
```

并在 `kRunners` 里登记：`{"tab.pin", TabPin},`。

**结果**：

| 入口 | 表现 |
| --- | --- |
| 菜单、命令面板 | 标签页右键「固定」和 `>` 固定，跟以前一样 |
| Pulse 助手 | 用户说「把前三个标签页固定」，助手调用 `run_actions [{"action":"tab.pin","args":{"tab":[1,2,3]}}]`，结果卡显示「固定了 3 个标签页」并带「撤销」 |
| MCP | 在「查看与界面」组里，默认开，直接执行；`undo_last` 可以撤销 |
| 检查 | `check_actions.py --strict` 通过 |

假如它是 External（比如「推送到远端」），还要把 id 加进 `kAskIds` 并写 `AskFor`。这样助手和 MCP 都会先弹确认卡，而 MCP 默认根本不允许它（「Pulse 之外的效果」组默认关）。
