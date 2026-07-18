package com.jcc.shell;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/** 连接游戏进程内 127.0.0.1:31338（注入成功后） */
public final class BridgeClient {
    private static final String HOST = "127.0.0.1";
    private static final int PORT = 31338;

    private BridgeClient() {}

    public static String request(String line, int timeoutMs) {
        Socket s = null;
        try {
            s = new Socket();
            s.connect(new InetSocketAddress(HOST, PORT), timeoutMs);
            s.setSoTimeout(timeoutMs);
            OutputStream os = s.getOutputStream();
            os.write((line.endsWith("\n") ? line : line + "\n").getBytes(StandardCharsets.UTF_8));
            os.flush();
            BufferedReader br = new BufferedReader(
                    new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            String resp = br.readLine();
            return resp != null ? resp : "";
        } catch (Exception e) {
            return null;
        } finally {
            if (s != null) {
                try {
                    s.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    public static String getCardPool() {
        return request("GET:牌库", 8000);
    }

    public static String getStatus() {
        String r = request("GET:STATUS", 3000);
        if (r == null) r = request("GET:日志", 3000);
        return r;
    }
}
