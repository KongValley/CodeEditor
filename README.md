# 内网安全代码编辑器（CodeEditor）

防记事本改坏文件的代码编辑器：**打开什么格式，保存后就是什么格式**——编码、BOM、换行符原样保持。

- 单文件 exe，零运行时依赖（静态链接 + msvcrt）
- Windows 7 SP1 / Windows 10，32/64 位通吃（32 位 exe，x64 经 WOW64）
- 语法高亮：ini/cfg/conf/properties、json/xml/yaml、sql、py/js/html/css/bat 等 29 种
- 编码检测：UTF-8 BOM / UTF-8 无 BOM / UTF-16LE / UTF-16BE / ANSI(GBK)，保存不转换
- 行尾保持：CRLF / LF / CR，粘贴不强制转换
- 自动缩进、查找/替换、行号、状态栏显示编码与行尾
- 查找增强：状态栏显示匹配数；全部替换前提示将替换的处数
- 安全：保存时自动生成 `.bak` 备份；文件被其他程序改动时会提示重新加载；保存后状态栏显示格式是否保持
- 视图：行尾符/空白可见（可关）；字号缩放（Ctrl+加/减/0）；JSON/XML 保存前结构检查（报行号）
- 编辑：按其他编码重新加载（UTF-8 / GBK / UTF-16LE）；最近打开文件列表

## 使用

双击 `dist\CodeEditor.exe`（或 `代码编辑器.exe`）即用，无需安装。

把文件拖进窗口、或命令行传文件路径均可打开。

## 自测

```
CodeEditor.exe --selftest   # 12 用例：字节级保持 + 备份 + 强制编码重载 + 格式校验 + 坏 MRU 值解析
CodeEditor.exe --uitest     # 27 用例：菜单命令/通知路径（缩放、行尾开关、编码重载、
                            #   外部修改检测三态、保存校验弹窗两态、全部替换预警两态、
                            #   最近文件菜单、关闭守卫、只读保存拦截、超长路径错误安全），弹窗脚本化应答
CodeEditor.exe --uicheck    # 窗口/Scintilla/着色/折叠链路（需指定一个含 "value" 的 json 文件）
```

结果分别写 `%TEMP%\CodeEditorSelftest\selftest-result.txt`、`%TEMP%\CodeEditorUiTest\uitest-result.txt`、`%TEMP%\CodeEditorUiCheck\uicheck-result.txt`，退出码 0 = 全过。

## 构建

```
build.bat
```

自动下载 MinGW-w64 i686（msvcrt）+ Scintilla 5.6.7 + Lexilla 5.5.4，编译静态库后链接出 `dist\CodeEditor.exe`。

构建必须在 ASCII 路径进行（GNU ld 在含中文的路径下解析库文件失败）。`build.bat` 使用 ASCII 构建农场 `C:\cedb`：源码同步过去、在那里编译、产物拷回本目录。

## 源码

`src/main.cxx` 单文件，Win32 + Scintilla/Lexilla 静态链接。

## 许可证

本项目源码采用 [MIT 许可证](LICENSE)。

本项目包含或链接以下第三方组件，其许可证声明见 [NOTICE.md](NOTICE.md)：
- **Scintilla** 5.6.7（编辑器控件）
- **Lexilla** 5.5.4（语法词法分析器）
- MinGW-w64（仅构建期工具链，不进入交付物）

