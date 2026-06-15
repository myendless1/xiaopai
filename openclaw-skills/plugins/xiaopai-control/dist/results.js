export function queuedCommandResult(command, details, deviceId) {
    return {
        status: "queued",
        ...(deviceId ? { device_id: deviceId } : {}),
        action: {
            type: "xiaopai.command",
            status: "success",
            details: {
                command_type: command.type,
                ...details
            }
        }
    };
}
export function rejectedCommandResult(error) {
    return {
        status: "rejected",
        action: {
            type: "xiaopai.command",
            status: "failed",
            details: {
                ...(error.field ? { field: error.field } : {}),
                ...(error.step_index === undefined ? {} : { step_index: error.step_index }),
                ...(error.value === undefined ? {} : { value: error.value })
            },
            error: {
                code: error.code,
                message: error.message
            }
        }
    };
}
export function failedCommandResult(commandType, code, message, details, deviceId) {
    return {
        status: "failed",
        ...(deviceId ? { device_id: deviceId } : {}),
        action: {
            type: "xiaopai.command",
            status: "failed",
            details: {
                command_type: commandType,
                ...(details ?? {})
            },
            error: {
                code,
                message
            }
        }
    };
}
//# sourceMappingURL=results.js.map