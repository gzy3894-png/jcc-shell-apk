package com.jcc.shell;

import android.app.Activity;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Root 注入壳：提取 JCC.sh + 我们的 libJCC.so → 启游戏 → 注入 → 连 31338 读牌库。
 * 不依赖 LSPosed / Zygisk 模块。
 */
public class MainActivity extends Activity {
    private static final String PKG = "com.tencent.jkchess";
    private static final String GAME_ACT =
            "com.tencent.jkchess/com.tencent.gcloud.msdk.core.policy.ZGamePolicyActivity";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private TextView tvLog;
    private TextView tvStatus;
    private TextView tvPool;
    private boolean emu;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        emu = Build.SUPPORTED_ABIS[0].contains("x86");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));
        root.setBackgroundColor(0xFF12141A);

        TextView title = new TextView(this);
        title.setText("JCC Shell (Root)");
        title.setTextColor(Color.WHITE);
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        tvStatus = new TextView(this);
        tvStatus.setText("● 未启动");
        tvStatus.setTextColor(0xFFFFCC00);
        tvStatus.setTextSize(16);
        tvStatus.setPadding(0, dp(8), 0, dp(8));
        root.addView(tvStatus);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button bLaunch = btn("一键注入", v -> io.execute(this::doLaunch));
        Button bPool = btn("刷新牌库", v -> io.execute(this::refreshPool));
        Button bClean = btn("清理", v -> io.execute(this::doClean));
        row.addView(bLaunch);
        row.addView(bPool);
        row.addView(bClean);
        root.addView(row);

        TextView h1 = new TextView(this);
        h1.setText("牌库预览");
        h1.setTextColor(0xFFAAAAAA);
        h1.setPadding(0, dp(12), 0, dp(4));
        root.addView(h1);

        ScrollView poolScroll = new ScrollView(this);
        tvPool = new TextView(this);
        tvPool.setTextColor(0xFFE0E0E0);
        tvPool.setTextSize(11);
        tvPool.setTypeface(Typeface.MONOSPACE);
        tvPool.setText("(注入成功后点「刷新牌库」)");
        poolScroll.addView(tvPool);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(180));
        root.addView(poolScroll, pp);

        TextView h2 = new TextView(this);
        h2.setText("日志");
        h2.setTextColor(0xFFAAAAAA);
        h2.setPadding(0, dp(8), 0, dp(4));
        root.addView(h2);

        ScrollView logScroll = new ScrollView(this);
        tvLog = new TextView(this);
        tvLog.setTextColor(0xFFB0BEC5);
        tvLog.setTextSize(12);
        tvLog.setTypeface(Typeface.MONOSPACE);
        logScroll.addView(tvLog);
        root.addView(logScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        io.execute(() -> {
            boolean rootOk = Su.hasRoot();
            log(rootOk ? "[+] Root OK" : "[-] 无 Root，无法注入");
            log(emu ? "[*] 模拟器 ABI" : "[*] 真机 ABI");
            setStatus(rootOk ? "● 就绪" : "● 需要 Root", rootOk ? 0xFF4CAF50 : 0xFFF44336);
        });
    }

    private Button btn(String t, View.OnClickListener c) {
        Button b = new Button(this);
        b.setText(t);
        b.setOnClickListener(c);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(dp(2), 0, dp(2), 0);
        b.setLayoutParams(lp);
        return b;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void log(String s) {
        ui.post(() -> {
            tvLog.append(s + "\n");
        });
    }

    private void setStatus(String s, int color) {
        ui.post(() -> {
            tvStatus.setText(s);
            tvStatus.setTextColor(color);
        });
    }

    private void extractAsset(String assetPath, String destName) throws Exception {
        AssetManager am = getAssets();
        File cache = new File(getCacheDir(), destName);
        try (InputStream in = am.open(assetPath);
             FileOutputStream out = new FileOutputStream(cache)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        String abs = cache.getAbsolutePath();
        Su.exec("cp '" + abs + "' /data/local/tmp/" + destName);
        Su.exec("chmod 755 /data/local/tmp/" + destName);
        // noinspection ResultOfMethodCallIgnored
        cache.delete();
    }

    private void doLaunch() {
        try {
            setStatus("● 启动中...", 0xFFFFCC00);
            log("=== JCC Shell Root 注入 ===");
            if (!Su.hasRoot()) {
                log("[-] 无 Root");
                setStatus("● 无 Root", 0xFFF44336);
                return;
            }

            log("[1] 提取注入器 + SO...");
            // 真机：JCC.sh 负责注入；我们的 libJCC.so 被其 dlopen
            extractAsset("JCC.sh", "JCC.sh");
            // NDK 产物优先：jniLibs 解压到 nativeLibraryDir
            File so = findLibJCC();
            if (so != null && so.exists()) {
                Su.exec("cp '" + so.getAbsolutePath() + "' /data/local/tmp/libJCC.so");
                Su.exec("chmod 755 /data/local/tmp/libJCC.so");
                log("    [+] libJCC.so (shell) -> /data/local/tmp/");
            } else {
                // 兜底：assets 里旧 so（原版，仅占位）
                extractAsset("emu/libJCC.so", "libJCC.so");
                log("    [!] 使用 assets 内置 so（可能非本季逻辑）");
            }
            if (emu) {
                extractAsset("emu/emu_injector", "emu_injector");
                extractAsset("emu/libJCC_helper.so", "libJCC_helper.so");
                log("    [+] 模拟器注入器已提取");
            }

            log("[2] 关闭游戏...");
            Su.exec("am force-stop " + PKG);
            Su.exec("kill -9 $(pidof JCC.sh) 2>/dev/null");
            sleep(1000);

            log("[3] 启动游戏...");
            Su.exec("am start -n " + GAME_ACT);
            String pid = null;
            for (int i = 0; i < 30; i++) {
                sleep(1000);
                String p = Su.exec("pidof " + PKG);
                if (p != null && p.matches("\\d+")) {
                    pid = p.trim();
                    log("    PID=" + pid);
                    break;
                }
            }
            if (pid == null) {
                log("[-] 游戏未起来");
                setStatus("● 游戏未启动", 0xFFF44336);
                return;
            }

            log("[4] 等 libil2cpp...");
            for (int i = 0; i < 60; i++) {
                sleep(1000);
                String m = Su.exec("grep libil2cpp.so /proc/" + pid + "/maps 2>/dev/null | head -1");
                if (m != null && m.contains("libil2cpp")) {
                    log("    引擎就绪 +" + (i + 1) + "s");
                    sleep(2000);
                    break;
                }
            }

            log("[5] 注入...");
            String inj;
            if (emu) {
                inj = Su.exec("/data/local/tmp/emu_injector -s /data/local/tmp/libJCC_helper.so");
            } else {
                inj = Su.exec("nsenter -t 1 -m -- /data/local/tmp/JCC.sh");
                if (inj == null || inj.contains("ERROR") || inj.isEmpty()) {
                    inj = Su.exec("/data/local/tmp/JCC.sh");
                }
            }
            if (inj != null && !inj.isEmpty()) log("    " + inj.replace("\n", "\n    "));

            sleep(3000);
            // 端口 31338 = 0x7A6A
            String tcp = Su.exec("cat /proc/$(pidof " + PKG + ")/net/tcp 2>/dev/null | grep -i 7A6A");
            if (tcp != null && !tcp.isEmpty() && !tcp.startsWith("ERROR")) {
                log("[+] 控制端口 31338 已监听");
                setStatus("● 运行中", 0xFF4CAF50);
            } else {
                // 也看状态文件
                String st = Su.exec("cat /data/user/0/" + PKG + "/files/jcc_shell_status.txt 2>/dev/null | tail -5");
                log("[*] 端口未确认，status:\n" + (st == null ? "(空)" : st));
                setStatus("● 已注入(待确认)", 0xFFFFCC00);
            }
            log("=== 完成：可点「刷新牌库」===");
            refreshPool();
        } catch (Exception e) {
            log("[-] " + e.getMessage());
            setStatus("● 失败", 0xFFF44336);
        }
    }

    private File findLibJCC() {
        // lib/arm64/libJCC.so from apk install path
        String[] abis = Build.SUPPORTED_ABIS;
        File nd = new File(getApplicationInfo().nativeLibraryDir);
        File f = new File(nd, "libJCC.so");
        if (f.exists()) return f;
        for (String abi : abis) {
            File alt = new File(getApplicationInfo().sourceDir);
            // also try extracted
        }
        return f.exists() ? f : null;
    }

    private void refreshPool() {
        log("[*] GET:牌库 ...");
        String r = BridgeClient.getCardPool();
        if (r == null) {
            // root 读文件兜底
            String file = Su.exec("cat /data/user/0/" + PKG + "/files/jcc_cardpool.txt 2>/dev/null");
            if (file != null && !file.isEmpty() && !file.startsWith("ERROR")) {
                log("[+] 从文件读取牌库 " + file.length() + " bytes");
                showPool(file);
                return;
            }
            String st = Su.exec("cat /data/user/0/" + PKG + "/files/jcc_shell_status.txt 2>/dev/null | tail -8");
            log("[-] 连不上 31338，status:\n" + (st == null ? "" : st));
            ui.post(() -> tvPool.setText("无数据（先一键注入，进大厅等扫表）"));
            return;
        }
        log("[+] " + (r.length() > 80 ? r.substring(0, 80) + "..." : r));
        String body = r.startsWith("RSP:") ? r.substring(4) : r;
        showPool(body);
    }

    private void showPool(String body) {
        // 只展示前若干条，避免卡 UI
        String[] parts = body.split(",");
        StringBuilder sb = new StringBuilder();
        sb.append("共 ").append(parts.length).append(" 条（预览前 40）\n\n");
        int n = Math.min(40, parts.length);
        for (int i = 0; i < n; i++) {
            sb.append(parts[i]).append('\n');
        }
        ui.post(() -> tvPool.setText(sb.toString()));
    }

    private void doClean() {
        log("=== 清理 ===");
        Su.exec("am force-stop " + PKG);
        Su.exec("rm -f /data/local/tmp/JCC.sh /data/local/tmp/libJCC.so /data/local/tmp/emu_injector /data/local/tmp/libJCC_helper.so");
        Su.exec("rm -f /data/user/0/" + PKG + "/files/jcc_cardpool.txt /data/user/0/" + PKG + "/files/jcc_shell_status.txt");
        log("[+] 完成");
        setStatus("● 已清理", 0xFF9E9E9E);
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }
}
