# Third-Party Notices

本软件（CodeEditor / 代码编辑器）的以下组件为第三方成果，按库各自许可证的要求，
随本软件分发时保留以下声明。

## Scintilla

    Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>
    All Rights Reserved

    Permission to use, copy, modify, and distribute this software and its
    documentation for any purpose and without fee is hereby granted,
    provided that the above copyright notice appear in all copies and that
    both that copyright notice and this permission notice appear in
    supporting documentation.

    NEIL HODGSON DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
    INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
    EVENT SHALL NEIL HODGSON BE LIABLE FOR ANY SPECIAL, INDIRECT OR
    CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
    USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
    OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
    PERFORMANCE OF THIS SOFTWARE.

项目主页：https://www.scintilla.org/
版本：5.6.7（以静态库形式链接进可执行文件）
用途：编辑器控件（文本缓冲、语法着色框架、字体渲染、行号边栏、折叠框架）

## Lexilla

    Copyright 1998-2021 by Neil Hodgson <neilh@scintilla.org>
    All Rights Reserved

    Permission to use, copy, modify, and distribute this software and its
    documentation for any purpose and without fee is hereby granted,
    provided that the above copyright notice appear in all copies and that
    both that copyright notice and this permission notice appear in
    supporting documentation.

    NEIL HODGSON DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
    INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
    EVENT SHALL NEIL HODGSON BE LIABLE FOR ANY SPECIAL, INDIRECT OR
    CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
    USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
    OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
    PERFORMANCE OF THIS SOFTWARE.

项目主页：https://scintilla.org/Lexilla.html
版本：5.5.4（以静态库形式链接进可执行文件）
用途：语法词法分析器集合（props/conf/json/xml/yaml/sql/batch/html/cpp/css/python/bash）

## MinGW-w64 工具链（仅构建期使用，不进入交付物）

交付的可执行文件由 MinGW-w64 GCC 13.2.0（i686-w64-mingw32，msvcrt 运行时）编译，
采用完全静态链接（-static -static-libgcc -static-libstdc++）。MinGW-w64 运行时
许可（MinGW-w64 runtime licensing: Public domain headers/import libraries with
BSD-like third-party components）不因二进制分发而触发附加义务；可执行文件本身
仅依赖操作系统自带的系统 DLL（kernel32/user32/gdi32/msvcrt 等 12 个）。
工具链仅用于本机构建，不随软件分发。

---

本项目源码（src/main.cxx 等）采用 MIT 许可证，见仓库根目录 LICENSE 文件。
