import { describe, expect, it } from "vitest";
import workAssistantPlugin from "../src/index.js";
import { sampleEvent } from "./helpers.js";

describe("plugin entry", () => {
  it("registers the gateway method without Lark side effects", async () => {
    const methods = new Map<string, (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void>();
    workAssistantPlugin.register({
      pluginConfig: { dryRun: true },
      registerGatewayMethod(name: string, handler: (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void) {
        methods.set(name, handler);
      }
    } as never);

    expect(methods.has("workAssistant.handleEvent")).toBe(true);
    const handler = methods.get("workAssistant.handleEvent");
    let payload: unknown;
    await handler?.({
      params: { event: sampleEvent },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      follow_up: { expected: false },
      actions: [{ type: "lark.calendar.create", status: "success" }]
    });
  });

  it("handles dry-run agenda briefing gateway events", async () => {
    const methods = new Map<string, (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void>();
    workAssistantPlugin.register({
      pluginConfig: { dryRun: true },
      registerGatewayMethod(name: string, handler: (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void) {
        methods.set(name, handler);
      }
    } as never);

    const handler = methods.get("workAssistant.handleEvent");
    let payload: unknown;
    await handler?.({
      params: { event: { ...sampleEvent, event_id: "fixture-head-touch-001", type: "head_touch" } },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      follow_up: { expected: false },
      actions: [
        { type: "lark.calendar.list", status: "success" },
        { type: "lark.calendar.list", status: "success" },
        { type: "agenda.summary.generate", status: "success" }
      ]
    });
  });

  it("handles dry-run wellbeing nudges with upcoming event context", async () => {
    const methods = new Map<string, (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void>();
    workAssistantPlugin.register({
      pluginConfig: { dryRun: true },
      registerGatewayMethod(name: string, handler: (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void) {
        methods.set(name, handler);
      }
    } as never);

    const handler = methods.get("workAssistant.handleEvent");
    let payload: unknown;
    await handler?.({
      params: {
        event: {
          ...sampleEvent,
          event_id: "fixture-sedentary-detected-001",
          type: "sedentary_detected",
          timestamp: "2026-06-06T13:40:00+08:00",
          payload: {
            duration_minutes: 35,
            confidence: 0.91
          }
        }
      },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      follow_up: { expected: true },
      actions: [
        { type: "lark.calendar.list", status: "success" },
        { type: "wellbeing.sedentary.evaluate", status: "success", details: { decision: "allowed" } }
      ],
      context_patch: {
        wellbeing_nearby_event: {
          title: "项目内部同步"
        }
      }
    });
  });

  it("handles dry-run wellbeing meeting-overlap skips", async () => {
    const methods = new Map<string, (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void>();
    workAssistantPlugin.register({
      pluginConfig: { dryRun: true },
      registerGatewayMethod(name: string, handler: (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void) {
        methods.set(name, handler);
      }
    } as never);

    const handler = methods.get("workAssistant.handleEvent");
    let payload: unknown;
    await handler?.({
      params: {
        event: {
          ...sampleEvent,
          event_id: "fixture-sedentary-meeting-overlap-001",
          type: "sedentary_detected",
          timestamp: "2026-06-06T14:10:00+08:00",
          payload: {
            duration_minutes: 35,
            confidence: 0.91
          }
        }
      },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      speech: "",
      follow_up: { expected: false },
      actions: [
        { type: "lark.calendar.list", status: "success" },
        { type: "wellbeing.sedentary.evaluate", status: "skipped", details: { decision: "meeting_overlap" } }
      ]
    });
  });
});
