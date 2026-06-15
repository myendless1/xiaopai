import { describe, expect, it } from "vitest";
import type { XiaopaiControlAdapter } from "../src/adapter.js";
import { createXiaopaiControlHandler } from "../src/handler.js";

describe("dry-run compatible handler behavior", () => {
  it("applies defaultDeviceId before adapter invocation", async () => {
    const adapter = new RecordingAdapter();
    const handler = createXiaopaiControlHandler({ adapter, defaultDeviceId: "robot-default" });

    await handler.execute({ type: "speak", text: "hello" });
    await handler.execute({ type: "speak", text: "hello", device_id: "robot-explicit" });

    expect(adapter.commands).toEqual([
      { type: "speak", text: "hello", device_id: "robot-default" },
      { type: "speak", text: "hello", device_id: "robot-explicit" }
    ]);
  });

  it("rejects invalid commands before adapter invocation", async () => {
    const adapter = new RecordingAdapter();
    const handler = createXiaopaiControlHandler({ adapter });

    const result = await handler.execute({ type: "face", expression: "unknown" });

    expect(adapter.commands).toEqual([]);
    expect(result).toMatchObject({
      status: "rejected",
      action: { error: { code: "unsupported_expression" } }
    });
  });

  it("accepts direct commands and wrapped command params", async () => {
    const adapter = new RecordingAdapter();
    const handler = createXiaopaiControlHandler({ adapter });

    await handler.execute({ type: "speak", text: "direct" });
    await handler.execute({ command: { type: "speak", text: "wrapped" } });

    expect(adapter.commands.map((command) => command.type === "speak" && command.text)).toEqual(["direct", "wrapped"]);
  });
});

class RecordingAdapter implements XiaopaiControlAdapter {
  public commands: unknown[] = [];

  async execute(command: unknown) {
    this.commands.push(command);
    return {
      status: "queued" as const,
      action: { type: "xiaopai.command" as const, status: "success" as const, details: { command_type: "test" } }
    };
  }

  async getHealth() {
    return { status: "ok" as const, reachable: true };
  }

  async listDevices() {
    return { status: "ok" as const, devices: [] };
  }
}
