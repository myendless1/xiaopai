import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiValidationError } from "./contracts.js";
export declare function queuedCommandResult(command: XiaopaiCommand, details: Record<string, unknown>, deviceId?: string): XiaopaiCommandResult;
export declare function rejectedCommandResult(error: XiaopaiValidationError): XiaopaiCommandResult;
export declare function failedCommandResult(commandType: string, code: string, message: string, details?: Record<string, unknown>, deviceId?: string): XiaopaiCommandResult;
