import type { XiaopaiControlAdapter } from "./adapter.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiDeviceListResult, XiaopaiHealthResult } from "./contracts.js";
import { rejectedResultForValidation, extractCommandInput, validateXiaopaiCommand } from "./validation.js";

export type XiaopaiControlHandlerOptions = {
  adapter: XiaopaiControlAdapter;
  defaultDeviceId?: string;
};

export type XiaopaiControlHandler = {
  execute(input: unknown): Promise<XiaopaiCommandResult>;
  getHealth(): Promise<XiaopaiHealthResult>;
  listDevices(): Promise<XiaopaiDeviceListResult>;
};

export function createXiaopaiControlHandler(options: XiaopaiControlHandlerOptions): XiaopaiControlHandler {
  return {
    async execute(input: unknown): Promise<XiaopaiCommandResult> {
      const validated = validateXiaopaiCommand(extractCommandInput(input));
      if (!validated.ok) return rejectedResultForValidation(validated.error);
      return options.adapter.execute(applyDefaultDeviceId(validated.value, options.defaultDeviceId));
    },
    getHealth() {
      return options.adapter.getHealth();
    },
    listDevices() {
      return options.adapter.listDevices();
    }
  };
}

export function applyDefaultDeviceId(command: XiaopaiCommand, defaultDeviceId: string | undefined): XiaopaiCommand {
  if (!defaultDeviceId || ("device_id" in command && command.device_id)) return command;
  return { ...command, device_id: defaultDeviceId } as XiaopaiCommand;
}
