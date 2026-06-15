import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiValidationError } from "./contracts.js";

export function queuedCommandResult(command: XiaopaiCommand, details: Record<string, unknown>, deviceId?: string): XiaopaiCommandResult {
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

export function rejectedCommandResult(error: XiaopaiValidationError): XiaopaiCommandResult {
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

export function failedCommandResult(commandType: string, code: string, message: string, details?: Record<string, unknown>, deviceId?: string): XiaopaiCommandResult {
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
