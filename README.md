# JCC Shell APK（Root 注入，不走 LSP / 不装 Zygisk 模块）

普通 APK + `su`：

1. 提取 `JCC.sh` + 我们的 `libJCC.so` 到 `/data/local/tmp`
2. 启动金铲铲，等 `libil2cpp`
3. 执行原版注入器把 SO `dlopen` 进游戏
4. SO 内用当前赛季字段扫 `SearchACGHero*`，起 `127.0.0.1:31338`
5. App 内「刷新牌库」拉数据；也可读  
   `/data/user/0/com.tencent.jkchess/files/jcc_cardpool.txt`

## 你需要

- Root
- 金铲铲 `com.tencent.jkchess`

## 构建

GitHub Actions → 产物 `jcc-shell-v1.0.0.apk`

## 若注入器与 SO 不匹配

把你手上的 **内存读写 / 注入源码** 丢过来，我按你的接口接 `cardpool` 扫表逻辑。
