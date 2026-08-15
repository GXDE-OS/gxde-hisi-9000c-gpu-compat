# gxde-hisi-9000c-gpu-compat

麒麟 9000c（HiSilicon Maleoon 910）GPU 在 GXDE 下的硬件加速兼容层。
配合 `gxde-wlcom (>= 2.1.6-gxde5)` 使用：由 `gxde-wlcom` 的启动脚本
`/usr/bin/startgxde_wlcom` 检测到 9000c 内核且存在
`/usr/bin/startgxde_wlcom_9000c` 时跳转到本包提供的硬件加速会话启动器，
未安装本包则回退到默认软件渲染流程。

> 适用范围：运行麒麟 `5.10.97-38-9000c` 系列内核的麒麟 9000c 设备。

---

## 背景：为什么需要这个包

麒麟 9000c 的 GPU（Maleoon 910）只有闭源驱动，且同时存在两套：

| 驱动 | 提供 | 位置 |
|---|---|---|
| `libGFX_hisi.so.0.8.0` | EGL / gbm / GLESv2 / wayland-egl（统一客户端+服务端） | `/usr/lib/aarch64-linux-gnu/`（由 `hw-maleoon-910` 提供） |
| `libhvgr.so` | Vulkan（官方 ICD） | `/etc/vulkan/icd.d/maleoon_vulkan.json` 指向 |

问题在于这两套驱动都不是为 Debian/GXDE 生态写的：

- hisi libEGL **不支持标准 EGL platform API**：没有 `eglGetPlatformDisplay`，
  `eglGetDisplay(gbm_device)` 直接 `EGL_BAD_DISPLAY`。wlroots/KWin 这类组合器
  全走标准 API，因此服务端根本无法用 hisi EGL —— 组合器只能退回 Mesa 软渲染，
  不通告 `wl_drm` / `zwp_linux_dmabuf_v1`，客户端的 hisi EGL 也跟着废。
- hisi 驱动只提供私有钩子 `egl_winsys_get_implementation_gbm`，只有 **UOS 打过
  补丁的 kwin_wayland** 会调用它（这也是当初 UOS chroot 里能跑通、GXDE 上跑不通
  的根本原因）。
- Vulkan 那边 `libhvgr` 在**旧内核**上 4G 显存预留 ioctl（`0xc010530e`）永远返回
  `EPERM`，与内核 hvgr UAPI 不匹配，Mesa→ZINK→libhvgr 的硬件路径也走不通。

结论：**必须是麒麟 9000c 内核（`5.10.97-38-9000c`）**，此时 hvgr UAPI 匹配、
驱动原生可用；用户态这边再补齐 libwayland / EGL / gbm 的兼容缺口，就是本包。

---

## 一路的艰辛（历史踩坑记录）

> 以下均为实测验证过的结论，供后续维护者理解"为什么是现在这个样子"，
> 不要轻易推翻，除非你能在真实 9000c 设备上复现并验证。

1. **libwayland 符号缺口**：hisi 驱动是按 UOS 的 libwayland 1.21 编译的，调用了
   Debian 1.23 没有的 `wl_proxy_ref` / `wl_proxy_unref` / `wl_proxy_get_refcount`。
   最初做了符号补丁 `libhisi_wl_shim.so`（`LD_PRELOAD` 拦截这三个符号）——驱动能
   加载了，但**假 refcount 外部表导致客户端段错误**，方案废弃。正确做法就是本包
   直接把 UOS 1.21 的 `libwayland-client/cursor` 库带进来，让 hisi 环境用真库。

2. **服务端 EGL 平台不兼容**：见上文背景。这解释了"为什么 wlcom 换任何环境变量
   都无法用 hisi 服务端渲染"——不是缺符号，是 hisi 根本不实现标准 EGL 平台 API。

3. **hvgr 4G 预留 EPERM 之谜**：`libhvgr` 与 `libGFX_hisi` 都会对 `/dev/hvgr0`
   发 rsv-add ioctl，旧内核一律 `EPERM`（root、持有 DRM master 都不行）。
   通过反汇编内核（kallsyms 全量可读 + `vmlinux-to-elf` 恢复符号）定位到：
   `hvgr_ioctl` 有 `ctx->flags bit2` 门控，只有 `hvgr_ioctl_cfg_driver`
   （`0x40305101`）能置位，而旧内核的 hvgr mem ioctl 表**根本不实现**
   `0xc010530e`（rsv-add），是纯 UAPI 版本错配。曾写 `hvgr_shim.so` 伪造
   cfg_driver + rsv-add 成功用于突破调试，但那是旧内核的临时手段，9000c 内核
   上**不再需要**任何 hvgr 补丁。

4. **换 9000c 内核后**：`vulkaninfo` / GLES / wlcom 全部免 shim 通过，
   `GL_RENDERER = Maleoon 910`，硬件桌面成立。

5. **客户端兼容补丁（本包的核心内容）**：即便服务端跑通，桌面上还有两类客户端
   问题需要 `libhisi_gbm_shim.so` 处理：
   - Qt6 绑 `wl_compositor` v6，而 UOS 1.21 的 `wl_surface_interface` 是 v5
     表，合成器下发 v6 事件时 libwayland 报
     `interface 'wl_surface' has no event 2`。通过 **数据符号拦截**重新导出
     `wl_surface_interface`（version=6、补上 `preferred_buffer_scale` /
     `preferred_buffer_transform` 两个事件）。
   - Qt5 wayland-egl 插件直接链接标准 `eglGetPlatformDisplay`，hisi 只有
     `eglGetPlatformDisplayEXT`（需 `eglGetProcAddress`），shim 负责转发。
   - Xwayland/glamor 需要的 `gbm_bo_create_with_modifiers2` /
     `gbm_bo_get_fd_for_plane` 由 shim 转发到旧版接口。

6. **命名约定**：会话启动器最初叫 `startgxde_wlcom_hisi9000c`，后按统一命名改为
   `startgxde_wlcom_9000c`。gxde-wlcom 侧对应版本为 `2.1.6-gxde5`
   （commit `b02d7707` / `5e1379b6`，tag `2.1.6-gxde5`）。
   ⚠️ GitHub 镜像仓库 `gxde-wlcom-src` 上还留有旧名方案的 commit（`281d0b6b`），
   与本包命名不一致，后续维护需同步。

---

## 包内容与工作原理

安装后提供：

```
/usr/bin/startgxde_wlcom_9000c                  # 硬件加速会话启动器
/usr/lib/gxde-hisi-9000c-gpu-compat/
├── libhisi_gbm_shim.so                         # LD_PRELOAD 兼容 shim（构建时编译）
├── libwayland-client.so.0.21.0                 # UOS libwayland 1.21（真实 refcount）
├── libwayland-cursor.so.0.21.0
├── xwayland-mesa                               # Xwayland 剥离 hisi 环境、走系统 Mesa
└── hisi_gbm_shim.c                             # shim 源码
```

调用链：

```
gxde-wlcom 的 /usr/bin/startgxde_wlcom
  └─ 检测: uname -r 含 "9000c" 且 /usr/bin/startgxde_wlcom_9000c 存在？
       ├─ 是 → export WLR_BACKENDS=drm,libinput; exec startgxde_wlcom_9000c
       └─ 否 → 原流程: exec gxde-wlcom -s startdde（软件渲染兜底）

startgxde_wlcom_9000c
  ├─ LD_LIBRARY_PATH=$HISI_DIR/server-egl      # hisi EGL/gbm/GLESv2 + UOS libwayland 1.21
  ├─ LD_PRELOAD=$HISI_DIR/libhisi_gbm_shim.so
  ├─ WLR_BACKENDS=drm,libinput                  # 防嵌套后端误选
  ├─ WLR_EGL_NO_MODIFIERS=1 / KYWC_EGL_NO_MODIFIERS=1
  ├─ WLR_XWAYLAND=$HISI_DIR/xwayland-mesa
  ├─ 清理陈旧 /tmp/.X*-lock；DISPLAY=:0
  └─ exec /usr/bin/gxde-wlcom -s /usr/bin/startdde
```

---

## 构建与安装

```bash
# 依赖: debhelper, gcc；生成上级目录的 gxde-hisi-9000c-gpu-compat_*.deb
./build.sh
# 或手动
dpkg-buildpackage -b -us -uc -d
```

CI（`.github/workflows/building.yml`）在推送 tag 时自动构建。

安装依赖：`gxde-wlcom (>= 2.1.6-gxde5)`、`hw-maleoon-910`（提供 hisi 驱动）、
`libwayland-client0`、`libwayland-cursor0`。

---

## 维护注意事项（务必逐条看）

1. **`server-egl/` 已随包提供**（`src/usr/lib/gxde-hisi-9000c-gpu-compat/server-egl/`），
   符号链接全部指向 `/usr/lib` 下的系统路径：
   - `libEGL.so[.1]` / `libgbm.so[.1]` / `libGLESv2.so[.2]` / `libwayland-egl.so[.1]`
     → `/usr/lib/aarch64-linux-gnu/libGFX_hisi.so.0.8.0`
   - `libwayland-client.so[.0]` / `libwayland-cursor.so[.0]`
     → `/usr/lib/gxde-hisi-9000c-gpu-compat/libwayland-client.so.0.21.0` 等
   ⚠ 保持绝对路径指向 `/usr/lib`：不要改成相对路径，也不要指向开发机的
   `~/hisi-override`（那是本机调试残留，打包会带坏）。`libwayland-egl.so.1`
   直接指 libGFX_hisi（UOS 的 libwayland-egl 就是 libGFX_hisi 的副本，md5 一致），
   不要额外打一个 3.9MB 的重复文件。

2. **绝对不要把 `libgbm.so.1` 重定向给 hisi 用在 Xwayland 上**。hisi libgbm 缺
   `gbm_bo_create_with_modifiers2` / `gbm_bo_get_fd_for_plane`，Xwayland 会因
   undefined symbol 崩溃。`xwayland-mesa` wrapper（unset LD_PRELOAD / LD_LIBRARY_PATH
   后再 exec Xwayland）是这个坑的官方解法，**不要去掉 `WLR_XWAYLAND`**。

3. **`WLR_EGL_NO_MODIFIERS=1` / `KYWC_EGL_NO_MODIFIERS=1` 不能省**，否则 hisi 拒绝
   仅含 `{INVALID}` 的 modifier 列表，初始化失败。

4. **`WLR_BACKENDS=drm,libinput` 要显式设置**。若环境里残留 `WAYLAND_DISPLAY` /
   `DISPLAY`，wlroots 会误选嵌套后端而不是 DRM，桌面起不来。这也是 gxde-wlcom 检测块
   里带这个变量的原因。

5. **启动前清理 `/tmp/.X*-lock` / `/tmp/.X11-unix/X*`**，否则 Xwayland 会挑 `:1`，
   DISPLAY 对不上。

6. **内核版本是硬前提**：必须是 `5.10.97-38-9000c` 系列。旧内核下 hvgr rsv-add
   永远 EPERM/EINVAL，用户态再怎么改都无解。不要在非 9000c 内核上折腾本包。

7. **命名必须同步**：gxde-wlcom 检测的路径 `/usr/bin/startgxde_wlcom_9000c`
   与本包安装路径必须一字不差。改名时两处（gxde-wlcom 的 `data/startgxde_wlcom`
   检测块 + 本包 `src/usr/bin/`）一起改，并同步 `debian/control` 的版本依赖描述。

8. **`libhisi_wl_shim.so`（旧符号 shim）已废弃**，不要再回退到它：假 refcount 会
   让客户端段错误。libwayland 1.23 缺符号的问题今后一直靠 UOS 1.21 库解决。

9. **shim 是构建时编译的**：`libhisi_gbm_shim.so` 由 `debian/rules` 从
   `hisi_gbm_shim.c` 编译生成。改动 shim 源码后必须重新 `./build.sh`，不要手工
   拷贝旧的 `.so` 进 `src/`。

10. **调试残留**：`~/hisi-override` 下的 `hisi_rsv_probe`、`master_holder`、
    `hvgr_shim.so`、`libhisi_wl_shim.so` 等是历史调试工具，9000c 内核上都不需要，
    不要把它们带进生产包。

11. **这套设备的 DRM UAPI 非 mainline**（例如 `SET_MASTER=DRM_IO(0x1e)`，mainline
    是 `DRM_IO(0x41)`；MODE 相关 ioctl 整体偏移到 0xA0+）。写内核/驱动相关测试时
    不要照搬 mainline 的 ioctl 号。

12. **本机手工验证流程**（真实设备）：
    ```bash
    # 文本终端 Ctrl+Alt+F3 登录后
    sudo systemctl stop gxdm
    /usr/bin/startgxde_wlcom_9000c
    # 另开终端查：
    glmark2-es2-wayland --gl-api=gles 2>&1 | grep -E 'GL_RENDERER|FPS'
    # GL_RENDERER 应为 Maleoon 910，而非 llvmpipe
    wayland-info | grep -E 'linux_dmabuf|wl_drm'   # 应有 zwp_linux_dmabuf_v1
    # 测完恢复
    sudo systemctl start gxdm
    ```

13. **IDE 编辑陷阱**：如果 `.c` 源文件在 IDE 里处于打开状态，通过其他方式编辑磁盘
    文件可能被编辑器缓冲覆盖。改 shim 源码后务必确认磁盘上的内容（`objdump -s` /
    `cat`）与预期一致，再构建。

14. **个别应用与 hisi 环境不兼容 → 用 `/usr/local/bin` 劫持启动脚本剥离环境**。
    会话级 `LD_LIBRARY_PATH=server-egl` + `LD_PRELOAD=libhisi_gbm_shim.so`
    对所有桌面进程生效，但少数应用会因此崩溃或打不开：
    - gxde-movie：mpv hwdec 探测加载 libhvgr 段错误 → `/usr/local/bin/gxde-movie`
    - obs-studio：OBS 需要桌面 OpenGL 3.3，hisi libEGL 只支持 OpenGL ES，
      `eglBindAPI(EGL_OPENGL_API)` 失败导致视频初始化失败 → `/usr/local/bin/obs`
    处理方式与 `xwayland-mesa` 相同：装一个 `/usr/local/bin/<app>` 脚本
    （`unset LD_PRELOAD; unset LD_LIBRARY_PATH; exec /usr/bin/<app> "$@"`，
    需要时再禁用 Vulkan ICD）。
    `.desktop` 的 `Exec=<app> %U` 是裸命令名，按 PATH 解析，`/usr/local/bin`
    优先于 `/usr/bin`，故无需改桌面文件。新增此类劫持时记住同步 changelog。

