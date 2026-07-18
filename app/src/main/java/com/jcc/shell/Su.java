package com.jcc.shell;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/** Root 执行（su） */
public final class Su {
    private Su() {}

    public static String exec(String cmd) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            StringBuilder out = new StringBuilder();
            BufferedReader br = new BufferedReader(
                    new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8));
            BufferedReader er = new BufferedReader(
                    new InputStreamReader(p.getErrorStream(), StandardCharsets.UTF_8));
            String line;
            while ((line = br.readLine()) != null) {
                if (out.length() > 0) out.append('\n');
                out.append(line);
            }
            while ((line = er.readLine()) != null) {
                if (out.length() > 0) out.append('\n');
                out.append(line);
            }
            p.waitFor();
            return out.toString().trim();
        } catch (Exception e) {
            return "ERROR:" + e.getMessage();
        } finally {
            if (p != null) p.destroy();
        }
    }

    public static boolean hasRoot() {
        String r = exec("id");
        return r != null && r.contains("uid=0");
    }

    /** 多行脚本 */
    public static String script(String body) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec("su");
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.write((body + "\nexit\n").getBytes(StandardCharsets.UTF_8));
            os.flush();
            StringBuilder out = new StringBuilder();
            BufferedReader br = new BufferedReader(
                    new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8));
            String line;
            while ((line = br.readLine()) != null) {
                if (out.length() > 0) out.append('\n');
                out.append(line);
            }
            p.waitFor();
            return out.toString().trim();
        } catch (Exception e) {
            return "ERROR:" + e.getMessage();
        } finally {
            if (p != null) p.destroy();
        }
    }
}
