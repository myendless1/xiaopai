import { rejectedResultForValidation, extractCommandInput, validateXiaopaiCommand } from "./validation.js";
export function createXiaopaiControlHandler(options) {
    return {
        async execute(input) {
            const validated = validateXiaopaiCommand(extractCommandInput(input));
            if (!validated.ok)
                return rejectedResultForValidation(validated.error);
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
export function applyDefaultDeviceId(command, defaultDeviceId) {
    if (!defaultDeviceId || ("device_id" in command && command.device_id))
        return command;
    return { ...command, device_id: defaultDeviceId };
}
//# sourceMappingURL=handler.js.map