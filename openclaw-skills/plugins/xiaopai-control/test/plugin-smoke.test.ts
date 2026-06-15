import { describe, expect, it } from "vitest";
import xiaopaiControlPlugin, { createDefaultXiaopaiControlRuntime } from "../src/index.js";
import { XiaopaiDryRunAdapter } from "../src/dry-run.js";
import { XiaopaiHttpAdapter } from "../src/http-adapter.js";

type GatewayHandler = (args: { params: Record<string, unknown>; respond: (ok: boolean, payload: unknown) => void }) => Promise<void> | void;

describe("xiaopai-control plugin entry", () => {
  it("registers execute, health, and device gateway methods with scopes", async () => {
    const methods = new Map<string, GatewayHandler>();
    const scopes = new Map<string, string>();

    xiaopaiControlPlugin.register({
      pluginConfig: { dryRun: true },
      registerGatewayMethod(name: string, handler: GatewayHandler, options?: { scope?: string }) {
        methods.set(name, handler);
        if (options?.scope) scopes.set(name, options.scope);
      }
    } as never);

    expect([...methods.keys()].sort()).toEqual([
      "tool.xiaopaiControl.execute",
      "xiaopaiControl.execute",
      "xiaopaiControl.getHealth",
      "xiaopaiControl.listDevices"
    ]);
    expect(scopes.get("tool.xiaopaiControl.execute")).toBe("operator.write");
    expect(scopes.get("xiaopaiControl.execute")).toBe("operator.write");
    expect(scopes.get("xiaopaiControl.getHealth")).toBe("operator.read");
    expect(scopes.get("xiaopaiControl.listDevices")).toBe("operator.read");

    let payload: unknown;
    await methods.get("xiaopaiControl.execute")?.({
      params: { command: { type: "speak", text: "hello" } },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      status: "queued",
      action: { type: "xiaopai.command", status: "success", details: { dry_run: true } }
    });

    await methods.get("tool.xiaopaiControl.execute")?.({
      params: { command: { type: "speak", text: "hello through alias" } },
      respond(ok, response) {
        expect(ok).toBe(true);
        payload = response;
      }
    });
    expect(payload).toMatchObject({
      status: "queued",
      action: { type: "xiaopai.command", status: "success", details: { dry_run: true } }
    });
  });

  it("selects the configured adapter type", () => {
    expect(createDefaultXiaopaiControlRuntime({ pluginConfig: { dryRun: true } }).adapter).toBeInstanceOf(XiaopaiDryRunAdapter);
    expect(createDefaultXiaopaiControlRuntime({ pluginConfig: { dryRun: false } }).adapter).toBeInstanceOf(XiaopaiHttpAdapter);
  });
});
