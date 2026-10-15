# Wayland 支持 / Wayland Support

[English](WAYLAND.md) | **简体中文**

open-vm-tools 13.1.0 的原生 Wayland 后端。剪贴板与拖放在 Wayland 会话下双向可用，
无需退回 X11 会话。

> 上游 open-vm-tools 的桌面集成强制运行在 XWayland 上（`copyPasteDnDX11.cpp` 中无条件调用
> `gdk_set_allowed_backends("x11")`），因此在 Wayland 会话中只能触及 Xwayland 的剪贴板，
> 而多数合成器拒绝未聚焦的 Xwayland 客户端读取它。本分支补上了原生后端。

- [功能概览](#功能概览)
- [各合成器支持情况](#各合成器支持情况)
- [构建](#构建)
- [运行前提](#运行前提)
- [环境变量](#环境变量)
- [相比社区补丁集的增强](#相比社区补丁集的增强)
- [已知限制](#已知限制)

## 功能概览

| 功能 | 说明 |
|---|---|
| 剪贴板（宿主 ↔ 客体） | 文本、RTF、PNG 图像、文件列表，双向 |
| 拖放 宿主 → 客体 | 合成器支持 layer shell 时走原生 Wayland 拖拽，否则经 Mutter 的 XDND 桥接 |
| 拖放 客体 → 宿主 | 原生 drop target，或在 X11 源上使用 XDND |
| 文件复制粘贴 | 客体 → 宿主直接传输；宿主 → 客体经 vmblock 惰性读取 |
| 自动分辨率 / 多显示器 | 沿用 `resolutionKMS`，并修复其在 Wayland 下被抢占的问题 |
| X11 会话 | 完全保留上游行为 |

剪贴板后端会同时声明所有可用格式，因此一次复制即可让 RTF 与纯文本并存——把富文本粘贴进
纯文本编辑器会得到纯文本，粘贴进富文本编辑器则保留格式。

## 各合成器支持情况

支持能力取决于合成器实现了哪些协议，而不取决于本程序。

| 合成器提供的协议 | 剪贴板 | 拖放 宿主 → 客体 | 拖放 客体 → 宿主 |
|---|---|---|---|
| `ext-data-control-v1` + `zwlr_layer_shell_v1` | 原生，全格式 | 原生 | 原生 |
| 仅 `ext-data-control-v1` | 原生，全格式 | 经 XDND 桥接 | 经 XDND 桥接 |
| 仅 `zwlr-data-control-unstable-v1` | 原生，全格式 | 经 XDND 桥接 | 经 XDND 桥接 |
| GNOME (Mutter)，无 data control | 经 Xwayland 的 X selection | 经 XDND 桥接 | 经 XDND 桥接 |
| 两者都无 | 不可用 | 仅 X11 应用 | 仅 X11 源 |

实测通过的环境：

| 环境 | 结果 |
|---|---|
| KDE Plasma 6.7.5 (KWin)，Wayland | 剪贴板 + 拖放，双向全格式 |
| niri 26.04，嵌套会话 | 剪贴板双向，含非 ASCII 文本 |
| GNOME (Mutter) | 官方补丁集已验证文本与文件 |

## 构建

### 依赖

除 open-vm-tools 常规依赖外，还需要：

- `wayland-client`（libwayland-client）
- `wayland-scanner`
- `wayland-protocols` **>= 1.39**（`ext-data-control-v1` 从这个版本开始提供）
- GTK 4 或 GTK 3 开发包（Wayland 后端随桌面插件一起构建）

Fedora / RHEL 系：

```bash
sudo dnf install gtkmm4.0-devel libsigc++30-devel glibmm2.68-devel \
    wayland-devel wayland-protocols-devel
```

Debian / Ubuntu 系：

```bash
sudo apt install libgtkmm-4.0-dev libsigc++-3.0-dev libglibmm-2.68-dev \
    libwayland-dev wayland-protocols
```

### 编译

```bash
autoreconf -i
./configure --with-gtk4 --with-wayland --enable-resolutionkms
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

Wayland 支持默认是 `auto`：找不到上述库时给出警告并构建为无 Wayland 支持。
显式指定时，缺失依赖会直接报错：

```bash
./configure --with-wayland     # 缺失依赖则报错
./configure --without-wayland  # 不带 Wayland 后端构建，行为同上游
```

未提供 `--with-gtk4` / `--with-gtk3` 时由 configure 自动探测，优先 GTK4。

> **构建提示：** 13.1.0 在 GCC 16 与 libsigc++ 3.8 下无法直接编译——后者移除了
> `signal<R, T...>` 旧语法并把它标记为 `final`。本分支已包含相应的兼容修复，无需额外补丁。

## 运行前提

1. **vmblock 服务已挂载**，宿主 → 客体的文件粘贴与文件拖放依赖它：

   ```bash
   sudo systemctl enable --now run-vmblock\\x2dfuse.mount
   ```

2. **vmusr 必须通过 `vmware-user-suid-wrapper` 启动**，而不是直接运行 `vmtoolsd -n vmusr`。
   该包装器以 root 权限打开 vmblock 与 `/dev/uinput` 这两个文件描述符并传给 vmusr；
   Wayland 下的拖放依赖 uinput 注入指针事件。

   发行版自带的 XDG 自启动项（`/etc/xdg/autostart/vmware-user.desktop`）已经这样做了。

3. **Xwayland 需要在运行**。vmusr 会加载上游的 desktopEvents 插件，它没有 X display 就
   无法启动。

4. 客体需要 `open-vm-tools-desktop` 包（桌面插件位于其中）。

**验证是否生效。** 后端接入时会写一条 debug 级日志（`g_debug`），
而默认日志级别不含 debug，需先在 `/etc/vmware-tools/tools.conf` 中开启：

```ini
[logging]
vmusr.level = debug
```

然后重启会话中的 vmusr（注销后重新登录，或 `systemctl --user restart vmtoolsd`
视发行版而定），日志中应出现绑定的协议之一：

```
WaylandClipboard_Init: using ext-data-control-v1
WaylandClipboard_Init: using zwlr_data_control_manager_v1
```

Linux 下 vmusr 日志默认写入 `/var/log/vmware-vmusr-<用户名>.log`。
日志里的 `no data control protocol is available` 表示合成器两个协议都不提供，
剪贴板会退回到 X selection 路径（仅 GNOME 上有效）。

> 更直接的验证：在客体中复制一段**含中文的文本**，粘贴到宿主。旧版本在这里必然失败
> （见下文 issue #800），能粘贴成功即说明后端已生效。

## 环境变量

| 变量 | 作用 |
|---|---|
| `VMTOOLS_DATA_CONTROL_PROTOCOL` | 强制指定剪贴板使用的 data control 协议，取值为 `ext_data_control_manager_v1` 或 `zwlr_data_control_manager_v1`。用于某合成器的某一个协议实现有问题、需要改用另一个时；也用于在同时提供两者的合成器上测试特定路径。 |

正常情况下不需要设置。两个协议都在时优先选用 `ext-data-control-v1`。

## 相比社区补丁集的增强

本分支的 Wayland 后端源自社区补丁集
[goproslowyo/ovt-wayland](https://github.com/goproslowyo/ovt-wayland)，在此之上做了三项补充：

1. **支持 `zwlr-data-control-v1`（老版合成器）。**
   社区补丁集只绑定 `ext-data-control-v1`，而该协议直到 wayland-protocols 1.39
   （2024 年）才出现。Plasma 5.27、sway 1.9 等长期支持版本只提供 `zwlr-data-control`，
   在这些系统上剪贴板此前完全不可用。本分支新增 `wayland-data-control.c` 适配层，
   用同一份实现驱动两个协议——两者接口、请求、事件顺序完全一致，差别仅在于创建对象时
   各自的接口指针，因此无需复制代码。两个协议都在时仍优先使用标准的 `ext-data-control-v1`。

2. **修复非 ASCII 文本剪贴板（上游 issue #800）。**
   `copyPasteUIX11GTK4.cpp` 曾把 `Glib::ustring::size()`（字符数）当作字节长度传给
   `CPClipboard_SetItem()`。任何含中文的文本复制到宿主都会失败；若截断点落在多字节序列
   中间则会整段丢弃。GTK3 版本用的正确写法是 `bytes()`。

3. **修复 Wayland 下多显示器失效（上游 issue #805）。**
   Wayland 会话中 `resolutionSet` 只能触及 Xwayland，其 RandR 输出无法设置，但它启动时
   会抢走 `resolutionKMS` 的 `resolution_server` 角色，导致宿主发出的
   `DisplayTopology_Set` 落到一个改不动输出的后端上，「循环切换多显示器」永久失效。
   现改为 Wayland 会话下主动退让。

另外还包含 Fedora 官方 RPM 使用的三个构建兼容补丁（GCC 16、libsigc++ 3.8、OpenSSL 4），
缺少它们 13.1.0 在当前的 Fedora 上无法编译。

## 已知限制

以下限制来自所继承的社区补丁集，尚未解决：

- **拖放起步过快会丢失。** 进入客体后约 100 ms 内松手，宿主会在客体获知指针位置前取消拖放。
- **GNOME 上拖放有延迟。** 宿主 → 客体的拖放约在指针进入客体半秒后才开始；更早松手会等待。
  GNOME 上完成一次客体 → 宿主拖放后，需先把指针移回客体一次，否则宿主会保持客体鼠标键按下状态。
- **显示缩放会偏移拖放落点。** VMware 窗口若缩放了客体显示，宿主 → 客体的拖放会落在指针旁边。
  使用与窗口匹配的客体分辨率（View → Autosize → Autofit Guest）可避免。
- **拖放文件有 2 分钟时限。** 应用在拖放结束两分钟后再读取被拖入的文件会找不到它
  （浏览器上传表单等场景）。先拖进文件管理器即可。
- **平铺合成器会拉伸检测窗口。** 需要把标题为 `vmware-user` 的窗口设为浮动，例如 sway 配置：
  `for_window [title="vmware-user"] floating enable`。
- **字体/格式细节。** 仅含 RTF 的剪贴板内容只能粘贴到接受 RTF 的应用中；宿主传给客体的文件名中的
  `:` 会被改写为 `^%`（Windows 宿主限制）。
- **粘贴的文件保留宿主权限**，只读文件到客体仍是只读（上游行为）。
- **分辨率相关的内核侧问题。** 宿主日志报 `guestScreens not congruent with hostScreens`
  时，可能是 `vmwgfx` 在 `DRM_VMW_UPDATE_LAYOUT` 后未更新已激活屏位置所致，属于内核驱动问题。
- **未使用 libei。** 指针注入走 `/dev/uinput`（由 suid 包装器以 root 打开），功能等效，
  但没有使用 Wayland 推荐的 libei 接口。
