import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiSequenceCommand } from "./contracts.js";
export declare const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export declare const XIAOPAI_EXECUTE_METHOD = "xiaopaiControl.execute";
export type XiaopaiRenderSkipReason = "not_stackchan" | "not_xiaopai_target" | "no_final_text" | "already_rendered" | "fallback_disabled";
export type XiaopaiRenderOutcome = "explicit_rendered" | "fallback_rendered" | "fallback_skipped" | "fallback_failed";
export type XiaopaiRenderContext = {
    required: true;
    device_id?: string;
    interrupt: boolean;
    event_id?: string;
    event_type?: string;
};
export type XiaopaiRenderDetection = {
    required: true;
    context: XiaopaiRenderContext;
} | {
    required: false;
    reason: XiaopaiRenderSkipReason;
};
export type XiaopaiRenderDiagnostic = {
    outcome: XiaopaiRenderOutcome;
    reason?: XiaopaiRenderSkipReason | "plugin_rejected" | "plugin_failed" | "exception";
    event_id?: string;
    device_id?: string;
    event_type?: string;
    details?: Record<string, unknown>;
};
export type XiaopaiRenderTurnState = {
    context: XiaopaiRenderContext;
    speechRendered: boolean;
    diagnostic?: XiaopaiRenderDiagnostic;
};
export type XiaopaiFallbackExecution = {
    command: XiaopaiSequenceCommand;
    result: XiaopaiCommandResult;
    diagnostic: XiaopaiRenderDiagnostic;
};
export declare function detectXiaopaiRenderIntentFromPrompt(prompt: unknown): XiaopaiRenderDetection;
export declare function detectXiaopaiRenderIntentFromMessages(messages: unknown[]): XiaopaiRenderDetection;
export declare function detectXiaopaiRenderIntentFromText(text: string): XiaopaiRenderDetection;
export declare function detectPlainTextStackchanRenderIntentFromText(text: string): XiaopaiRenderDetection;
export declare function detectXiaopaiRenderIntentFromSessionKey(sessionKey: unknown): XiaopaiRenderDetection;
export declare function isXiaopaiExecuteTool(toolName: unknown): boolean;
export declare function isSuccessfulXiaopaiCommandResult(result: unknown): result is XiaopaiCommandResult & {
    status: "queued";
};
export declare function commandContainsSpeech(input: unknown): boolean;
export declare function observeXiaopaiExecuteCall(params: unknown, result: unknown): boolean;
export declare function observeXiaopaiExecuteCliCall(params: unknown, result: unknown): boolean;
export declare function normalizeFallbackSpeechText(text: unknown): string;
export declare function buildFallbackSequenceCommand(context: XiaopaiRenderContext, speechText: string): XiaopaiSequenceCommand;
export declare function executeXiaopaiRenderFallback(options: {
    state: XiaopaiRenderTurnState;
    finalText: unknown;
    execute: (input: {
        command: XiaopaiCommand;
    }) => Promise<XiaopaiCommandResult>;
}): Promise<XiaopaiFallbackExecution | XiaopaiRenderDiagnostic>;
export declare function diagnosticBase(context: XiaopaiRenderContext): Omit<XiaopaiRenderDiagnostic, "outcome">;
export declare function sanitizeFailureDetails(value: unknown): Record<string, unknown>;
