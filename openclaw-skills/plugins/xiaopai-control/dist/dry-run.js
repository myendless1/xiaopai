import { queuedCommandResult } from "./results.js";
export class XiaopaiDryRunAdapter {
    async execute(command) {
        return queuedCommandResult(command, {
            cmd_id: `dry_run_${command.type}`,
            dry_run: true,
            stack_command_type: command.type === "action" ? "face" : command.type
        }, "device_id" in command ? command.device_id : undefined);
    }
    async getHealth() {
        return {
            status: "ok",
            reachable: true,
            service: "xiaopai-dry-run",
            details: {
                dry_run: true
            }
        };
    }
    async listDevices() {
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
//# sourceMappingURL=dry-run.js.map