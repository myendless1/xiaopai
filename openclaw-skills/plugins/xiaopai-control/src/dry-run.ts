import type { XiaopaiControlAdapter } from "./adapter.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
import { queuedCommandResult } from "./results.js";

export class XiaopaiDryRunAdapter implements XiaopaiControlAdapter {
  async execute(command: XiaopaiCommand): Promise<XiaopaiCommandResult> {
    return queuedCommandResult(
      command,
      {
        cmd_id: `dry_run_${command.type}`,
        dry_run: true,
        stack_command_type: command.type === "action" ? "face" : command.type
      },
      "device_id" in command ? command.device_id : undefined
    );
  }

  async getHealth(): Promise<XiaopaiHealthResult> {
    return {
      status: "ok",
      reachable: true,
      service: "xiaopai-dry-run",
      details: {
        dry_run: true
      }
    };
  }

  async listDevices(): Promise<XiaopaiDeviceListResult> {
    return {
      status: "ok",
      default_device_id: "dry-run-device",
      online_ttl_seconds: 90,
      devices: [
        {
          device_id: "dry-run-device",
          online: true,
          pending_commands: 0,
          last_seen_seconds_ago: 0
        }
      ]
    };
  }
}
