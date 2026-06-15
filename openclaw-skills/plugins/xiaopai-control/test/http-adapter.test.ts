import { describe, expect, it } from "vitest";
import { XiaopaiHttpAdapter } from "../src/http-adapter.js";

describe("XiaopaiHttpAdapter", () => {
  it("translates speak commands to POST /command and normalizes queued responses", async () => {
    const calls: Array<{ url: string; init: RequestInit }> = [];
    const adapter = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local/",
      timeoutMs: 1000,
      fetch: async (url, init) => {
        calls.push({ url: String(url), init: init ?? {} });
        return jsonResponse({
          type: "queued",
          device_id: "robot-1",
          command: { cmd_id: "cmd_123", type: "speak", interrupt: true }
        });
      }
    });

    const result = await adapter.execute({ type: "speak", text: "hello", device_id: "robot-1", interrupt: true });

    expect(calls).toHaveLength(1);
    expect(calls[0]?.url).toBe("http://stack.local/command");
    expect(calls[0]?.init.method).toBe("POST");
    expect(JSON.parse(String(calls[0]?.init.body))).toEqual({
      type: "speak",
      device_id: "robot-1",
      interrupt: true,
      payload: { text: "hello" }
    });
    expect(result).toMatchObject({
      status: "queued",
      device_id: "robot-1",
      action: {
        type: "xiaopai.command",
        status: "success",
        details: { command_type: "speak", cmd_id: "cmd_123" }
      }
    });
  });

  it("translates action, move, sequence, and stop requests", async () => {
    const calls: Array<{ url: string; init: RequestInit }> = [];
    const adapter = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local",
      timeoutMs: 1000,
      fetch: async (url, init) => {
        calls.push({ url: String(url), init: init ?? {} });
        return jsonResponse({
          type: "queued",
          device_id: "robot-1",
          command: { cmd_id: `cmd_${calls.length}`, type: "queued", interrupt: false }
        });
      }
    });

    await adapter.execute({ type: "action", action: "blink" });
    await adapter.execute({ type: "move", direction: "center" });
    await adapter.execute({
      type: "sequence",
      steps: [
        { type: "face", expression: "thinking" },
        { type: "action", action: "wink" },
        { type: "move", direction: "left", degree: 15, duration_ms: 500 },
        { type: "speak", text: "hi" }
      ]
    });
    await adapter.execute({ type: "stop", device_id: "robot-1" });

    expect(JSON.parse(String(calls[0]?.init.body))).toEqual({
      type: "action",
      payload: { expression: "blink" }
    });
    expect(JSON.parse(String(calls[1]?.init.body))).toEqual({
      type: "move",
      payload: { type: "center" }
    });
    expect(JSON.parse(String(calls[2]?.init.body))).toEqual({
      type: "sequence",
      payload: [
        { type: "face", expression: "thinking" },
        { type: "face", expression: "wink" },
        { type: "move", action: "left", degree: 15, duration_ms: 500 },
        { type: "speak", text: "hi" }
      ]
    });
    expect(calls[3]?.url).toBe("http://stack.local/command/stop?device_id=robot-1");
    expect(calls[3]?.init.method).toBe("GET");
  });

  it("normalizes HTTP, network, and malformed response failures", async () => {
    const httpFailure = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local",
      timeoutMs: 1000,
      fetch: async () => jsonResponse({ type: "error", message: "bad" }, 503)
    });
    await expect(httpFailure.execute({ type: "speak", text: "hello" })).resolves.toMatchObject({
      status: "failed",
      action: { error: { code: "http_error" } }
    });

    const networkFailure = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local",
      timeoutMs: 1000,
      fetch: async () => {
        throw new Error("connect http://stack.local failed");
      }
    });
    await expect(networkFailure.execute({ type: "speak", text: "hello" })).resolves.toMatchObject({
      status: "failed",
      action: { error: { code: "network_error", message: "connect [url] failed" } }
    });

    const malformed = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local",
      timeoutMs: 1000,
      fetch: async () => new Response("not json", { status: 200 })
    });
    await expect(malformed.execute({ type: "speak", text: "hello" })).resolves.toMatchObject({
      status: "failed",
      action: { error: { code: "malformed_response" } }
    });
  });

  it("normalizes health checks and device lists", async () => {
    const adapter = new XiaopaiHttpAdapter({
      baseUrl: "http://stack.local",
      timeoutMs: 1000,
      fetch: async (url) => {
        if (String(url).endsWith("/health")) {
          return jsonResponse({ ok: true, service: "xiaopai-aliyun-voice", openclaw: { enabled: true } });
        }
        return jsonResponse({
          type: "devices",
          default_device_id: "robot-1",
          online_ttl_seconds: 90,
          devices: [{ device_id: "robot-1", online: true, pending_commands: 2, last_seen_seconds_ago: 1.2 }]
        });
      }
    });

    await expect(adapter.getHealth()).resolves.toMatchObject({
      status: "ok",
      reachable: true,
      service: "xiaopai-aliyun-voice",
      details: { openclaw_enabled: true }
    });
    await expect(adapter.listDevices()).resolves.toMatchObject({
      status: "ok",
      default_device_id: "robot-1",
      devices: [{ device_id: "robot-1", online: true, pending_commands: 2 }]
    });
  });
});

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" }
  });
}
