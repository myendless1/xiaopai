import { describe, expect, it, vi } from "vitest";
import { createXiaopaiControlHandler } from "../src/handler.js";
import { registerXiaopaiRenderFallbackHooks } from "../src/index.js";
import {
  buildFallbackSequenceCommand,
  commandContainsSpeech,
  detectPlainTextStackchanRenderIntentFromText,
  detectXiaopaiRenderIntentFromText,
  detectXiaopaiRenderIntentFromSessionKey,
  executeXiaopaiRenderFallback,
  normalizeFallbackSpeechText,
  observeXiaopaiExecuteCall,
  observeXiaopaiExecuteCliCall
} from "../src/render-fallback.js";
import type { XiaopaiCommand, XiaopaiCommandResult } from "../src/contracts.js";
import { queuedCommandResult, failedCommandResult, rejectedCommandResult } from "../src/results.js";
import type { XiaopaiControlAdapter } from "../src/adapter.js";

describe("xiaopai render fallback helpers", () => {
  it("detects stack-chan Xiaopai render envelopes and extracts context", () => {
    const detection = detectXiaopaiRenderIntentFromText(
      JSON.stringify({
        schema: "openclaw.stackchan.event.v1",
        event_id: "evt-1",
        device_id: " outer-device ",
        event: {
          type: "user_utterance",
          payload: { device_id: "payload-device" }
        },
        render: { target: "xiaopai", interrupt: false }
      })
    );

    expect(detection).toEqual({
      required: true,
      context: {
        required: true,
        device_id: "outer-device",
        interrupt: false,
        event_id: "evt-1",
        event_type: "user_utterance"
      }
    });
  });

  it("ignores invalid JSON and non-Xiaopai stack-chan envelopes", () => {
    expect(detectXiaopaiRenderIntentFromText("{nope").required).toBe(false);
    expect(
      detectXiaopaiRenderIntentFromText(
        JSON.stringify({
          schema: "openclaw.stackchan.event.v1",
          event: { type: "user_utterance" },
          render: { target: "chat" }
        })
      )
    ).toEqual({ required: false, reason: "not_xiaopai_target" });
  });

  it("detects plain-text stack-chan system prompts", () => {
    expect(
      detectPlainTextStackchanRenderIntentFromText(
        "你是小派同学。stack-chan 会把小派收到的语音识别文本直接作为用户消息发给你；非触摸设备事件会作为一行简短的自然语言事件说明发给你。需要说话、移动或切换表情时，调用 xiaopaiControl.execute 渲染到当前小派设备。"
      )
    ).toEqual({
      required: true,
      context: {
        required: true,
        interrupt: true,
        event_type: "plain_text_stackchan"
      }
    });
  });

  it("detects stack-chan Xiaopai session keys", () => {
    expect(detectXiaopaiRenderIntentFromSessionKey("xiaopai-44-1b")).toMatchObject({
      required: true,
      context: { event_type: "plain_text_stackchan", device_id: "44-1b" }
    });
    expect(detectXiaopaiRenderIntentFromSessionKey("openai-user:xiaopai-44:1b:f6:e4:83:8c")).toMatchObject({
      required: true,
      context: { event_type: "plain_text_stackchan", device_id: "44:1b:f6:e4:83:8c" }
    });
    expect(detectXiaopaiRenderIntentFromSessionKey("openai-user:normal-chat")).toEqual({
      required: false,
      reason: "not_stackchan"
    });
  });

  it("normalizes direct and wrapped speech-capable commands", () => {
    expect(commandContainsSpeech({ type: "speak", text: "hello" })).toBe(true);
    expect(commandContainsSpeech({ command: { type: "sequence", steps: [{ type: "face", expression: "calm" }, { type: "speak", text: "hello" }] } })).toBe(true);
    expect(commandContainsSpeech({ type: "sequence", steps: [{ type: "face", expression: "calm" }] })).toBe(false);
    expect(commandContainsSpeech({ type: "face", expression: "calm" })).toBe(false);
  });

  it("marks only successful speech commands as explicit render", () => {
    const speak = { command: { type: "speak", text: "hello" } };
    const face = { command: { type: "face", expression: "calm" } };
    expect(observeXiaopaiExecuteCall(speak, queuedCommandResult(speak.command as XiaopaiCommand, {}))).toBe(true);
    expect(observeXiaopaiExecuteCall(face, queuedCommandResult(face.command as XiaopaiCommand, {}))).toBe(false);
    expect(observeXiaopaiExecuteCall(speak, failedCommandResult("speak", "adapter_failed", "boom"))).toBe(false);
    expect(
      observeXiaopaiExecuteCall(
        speak,
        rejectedCommandResult({
          code: "invalid_speech_text",
          message: "bad"
        })
      )
    ).toBe(false);
  });

  it("marks successful CLI gateway speech commands as explicit render", () => {
    const command =
      "openclaw gateway call xiaopaiControl.execute --json --params '{\"command\":{\"type\":\"sequence\",\"steps\":[{\"type\":\"face\",\"expression\":\"thinking\"},{\"type\":\"speak\",\"text\":\"hello\"}]}}'";
    const result = {
      details: {
        status: "completed",
        exitCode: 0,
        aggregated: JSON.stringify({
          status: "queued",
          device_id: "device-1",
          action: { type: "xiaopai.command", status: "success" }
        })
      }
    };
    expect(observeXiaopaiExecuteCliCall({ command }, result)).toBe(true);
    expect(observeXiaopaiExecuteCliCall({ command: command.replace("speak", "face") }, result)).toBe(false);
    expect(observeXiaopaiExecuteCliCall({ command }, failedCommandResult("sequence", "adapter_failed", "boom"))).toBe(false);
  });

  it("normalizes fallback text and fits the speech limit", () => {
    expect(normalizeFallbackSpeechText("assistant: hello\nDEBUG: hidden\nworld")).toBe("hello world");
    expect(normalizeFallbackSpeechText("hello MEDIA:fallback-rendered world")).toBe("hello world");
    expect(normalizeFallbackSpeechText(JSON.stringify({ diagnostic: true }))).toBe("");
    expect(normalizeFallbackSpeechText("a".repeat(800))).toHaveLength(500);
  });

  it("builds fallback sequence with device id and interrupt", () => {
    expect(
      buildFallbackSequenceCommand(
        {
          required: true,
          device_id: "device-1",
          interrupt: false
        },
        "hello"
      )
    ).toEqual({
      type: "sequence",
      device_id: "device-1",
      interrupt: false,
      steps: [
        { type: "speak", text: "hello" },
        { type: "face", expression: "calm" }
      ]
    });
  });

  it("executes fallback through the provided validated execution boundary", async () => {
    const execute = vi.fn(async (input: { command: XiaopaiCommand }) => queuedCommandResult(input.command, { dry_run: true }));
    const result = await executeXiaopaiRenderFallback({
      state: {
        context: {
          required: true,
          device_id: "device-1",
          interrupt: true,
          event_id: "evt-1"
        },
        speechRendered: false
      },
      finalText: "hello",
      execute
    });

    expect("command" in result && result.command).toMatchObject({
      type: "sequence",
      device_id: "device-1",
      interrupt: true
    });
    expect("command" in result && result.command.steps[0]).toEqual({ type: "speak", text: "hello" });
    expect("diagnostic" in result && result.diagnostic).toMatchObject({
      outcome: "fallback_rendered",
      event_id: "evt-1",
      device_id: "device-1"
    });
    expect(execute).toHaveBeenCalledTimes(1);
  });

  it("skips fallback when final text is empty or already explicitly rendered", async () => {
    const execute = vi.fn(async (input: { command: XiaopaiCommand }) => queuedCommandResult(input.command, {}));
    await expect(
      executeXiaopaiRenderFallback({
        state: { context: { required: true, interrupt: true }, speechRendered: false },
        finalText: "",
        execute
      })
    ).resolves.toMatchObject({ outcome: "fallback_skipped", reason: "no_final_text" });
    await expect(
      executeXiaopaiRenderFallback({
        state: { context: { required: true, interrupt: true }, speechRendered: true },
        finalText: "hello",
        execute
      })
    ).resolves.toMatchObject({ outcome: "fallback_skipped", reason: "already_rendered" });
    expect(execute).not.toHaveBeenCalled();
  });
});

describe("xiaopai render fallback hooks", () => {
  it("queues a dry-run fallback command for a text-only stack-chan turn", async () => {
    const executed: XiaopaiCommand[] = [];
    const adapter: XiaopaiControlAdapter = {
      async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
        executed.push(command);
        return queuedCommandResult(command, { dry_run: true });
      },
      async getHealth() {
        return { status: "ok", reachable: true };
      },
      async listDevices() {
        return { status: "ok", devices: [] };
      }
    };
    const handler = createXiaopaiControlHandler({ adapter });
    const hooks = new Map<string, (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown>();
    const logs: string[] = [];

    registerXiaopaiRenderFallbackHooks(
      {
        registerHook(name: string, hook: (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown) {
          hooks.set(name, hook);
        },
        logger: {
          debug: (message: string) => logs.push(message),
          info: (message: string) => logs.push(message),
          warn: (message: string) => logs.push(message),
          error: (message: string) => logs.push(message)
        }
      } as never,
      handler
    );

    const ctx = { runId: "run-1", sessionKey: "session-1" };
    await hooks.get("before_agent_run")?.(
      {
        prompt: JSON.stringify({
          schema: "openclaw.stackchan.event.v1",
          event_id: "evt-1",
          device_id: "device-1",
          event: { type: "user_utterance" },
          render: { target: "xiaopai", interrupt: true }
        }),
        messages: []
      },
      ctx
    );
    await hooks.get("before_agent_finalize")?.({ runId: "run-1", lastAssistantMessage: "hello from chat" }, ctx);

    expect(executed).toHaveLength(1);
    expect(executed[0]).toMatchObject({
      type: "sequence",
      device_id: "device-1"
    });
    expect(executed[0]?.type === "sequence" && executed[0].steps[0]).toEqual({ type: "speak", text: "hello from chat" });
    expect(logs.join("\n")).toContain("fallback_rendered");
  });

  it("queues fallback for a plain-text stack-chan turn detected from system prompt", async () => {
    const executed: XiaopaiCommand[] = [];
    const adapter: XiaopaiControlAdapter = {
      async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
        executed.push(command);
        return queuedCommandResult(command, { dry_run: true });
      },
      async getHealth() {
        return { status: "ok", reachable: true };
      },
      async listDevices() {
        return { status: "ok", devices: [] };
      }
    };
    const handler = createXiaopaiControlHandler({ adapter });
    const hooks = new Map<string, (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown>();

    registerXiaopaiRenderFallbackHooks(
      {
        registerHook(name: string, hook: (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown) {
          hooks.set(name, hook);
        },
        logger: {
          debug: vi.fn(),
          info: vi.fn(),
          warn: vi.fn(),
          error: vi.fn()
        }
      } as never,
      handler
    );

    const ctx = { runId: "run-plain", sessionKey: "xiaopai-plain" };
    await hooks.get("before_agent_run")?.(
      {
        messages: [
          {
            role: "system",
            content:
              "你是小派同学。stack-chan 会把小派收到的语音识别文本直接作为用户消息发给你；非触摸设备事件会作为一行简短的自然语言事件说明发给你。需要说话、移动或切换表情时，调用 xiaopaiControl.execute 渲染到当前小派设备。"
          },
          { role: "user", content: "你好，小派。" }
        ]
      },
      ctx
    );
    await hooks.get("before_agent_finalize")?.({ runId: "run-plain", lastAssistantMessage: "你好，我在。" }, ctx);

    expect(executed).toHaveLength(1);
    expect(executed[0]).toMatchObject({ type: "sequence" });
    expect(executed[0]?.type === "sequence" && executed[0].steps[0]).toEqual({ type: "speak", text: "你好，我在。" });
  });

  it("queues fallback for a plain-text stack-chan turn detected from session key", async () => {
    const executed: XiaopaiCommand[] = [];
    const adapter: XiaopaiControlAdapter = {
      async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
        executed.push(command);
        return queuedCommandResult(command, { dry_run: true });
      },
      async getHealth() {
        return { status: "ok", reachable: true };
      },
      async listDevices() {
        return { status: "ok", devices: [] };
      }
    };
    const handler = createXiaopaiControlHandler({ adapter });
    const hooks = new Map<string, (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown>();

    registerXiaopaiRenderFallbackHooks(
      {
        registerHook(name: string, hook: (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown) {
          hooks.set(name, hook);
        },
        logger: {
          debug: vi.fn(),
          info: vi.fn(),
          warn: vi.fn(),
          error: vi.fn()
        }
      } as never,
      handler
    );

    const ctx = { runId: "run-session", sessionKey: "openai-user:xiaopai-device-1" };
    await hooks.get("before_agent_run")?.({ messages: [{ role: "user", content: "你好，小派。" }] }, ctx);
    await hooks.get("before_agent_finalize")?.({ runId: "run-session", lastAssistantMessage: "你好，我在。" }, ctx);

    expect(executed).toHaveLength(1);
    expect(executed[0]).toMatchObject({ type: "sequence", device_id: "device-1" });
    expect(executed[0]?.type === "sequence" && executed[0].steps[0]).toEqual({ type: "speak", text: "你好，我在。" });
  });

  it("does not fallback when a successful explicit speak call was observed", async () => {
    const executed: XiaopaiCommand[] = [];
    const adapter: XiaopaiControlAdapter = {
      async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
        executed.push(command);
        return queuedCommandResult(command, { dry_run: true });
      },
      async getHealth() {
        return { status: "ok", reachable: true };
      },
      async listDevices() {
        return { status: "ok", devices: [] };
      }
    };
    const handler = createXiaopaiControlHandler({ adapter });
    const hooks = new Map<string, (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown>();

    registerXiaopaiRenderFallbackHooks(
      {
        registerHook(name: string, hook: (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown) {
          hooks.set(name, hook);
        },
        logger: {
          info: vi.fn(),
          warn: vi.fn(),
          error: vi.fn()
        }
      } as never,
      handler
    );

    const ctx = { runId: "run-2", sessionKey: "session-2" };
    await hooks.get("before_agent_run")?.(
      {
        prompt: JSON.stringify({
          schema: "openclaw.stackchan.event.v1",
          event: { type: "user_utterance" },
          render: { target: "xiaopai" }
        }),
        messages: []
      },
      ctx
    );
    await hooks.get("after_tool_call")?.(
      {
        toolName: "xiaopaiControl.execute",
        params: { command: { type: "speak", text: "explicit" } },
        result: queuedCommandResult({ type: "speak", text: "explicit" }, {})
      },
      ctx
    );
    await hooks.get("before_agent_finalize")?.({ runId: "run-2", lastAssistantMessage: "fallback text" }, ctx);

    expect(executed).toHaveLength(0);
  });

  it("does not fallback when a successful CLI execute speak call was observed", async () => {
    const executed: XiaopaiCommand[] = [];
    const adapter: XiaopaiControlAdapter = {
      async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
        executed.push(command);
        return queuedCommandResult(command, { dry_run: true });
      },
      async getHealth() {
        return { status: "ok", reachable: true };
      },
      async listDevices() {
        return { status: "ok", devices: [] };
      }
    };
    const handler = createXiaopaiControlHandler({ adapter });
    const hooks = new Map<string, (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown>();

    registerXiaopaiRenderFallbackHooks(
      {
        registerHook(name: string, hook: (event: Record<string, unknown>, ctx: Record<string, unknown>) => unknown) {
          hooks.set(name, hook);
        },
        logger: {
          info: vi.fn(),
          warn: vi.fn(),
          error: vi.fn()
        }
      } as never,
      handler
    );

    const ctx = { runId: "run-cli", sessionKey: "session-cli" };
    await hooks.get("before_agent_run")?.(
      {
        prompt: JSON.stringify({
          schema: "openclaw.stackchan.event.v1",
          device_id: "device-1",
          event: { type: "user_utterance" },
          render: { target: "xiaopai" }
        }),
        messages: []
      },
      ctx
    );
    await hooks.get("after_tool_call")?.(
      {
        toolName: "exec",
        params: {
          command:
            "openclaw gateway call xiaopaiControl.execute --json --params '{\"command\":{\"type\":\"sequence\",\"device_id\":\"device-1\",\"steps\":[{\"type\":\"speak\",\"text\":\"explicit\"}]}}'"
        },
        result: {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                status: "queued",
                device_id: "device-1",
                action: { type: "xiaopai.command", status: "success" }
              })
            }
          ]
        }
      },
      ctx
    );
    await hooks.get("before_agent_finalize")?.({ runId: "run-cli", lastAssistantMessage: "fallback text" }, ctx);

    expect(executed).toHaveLength(0);
  });
});
