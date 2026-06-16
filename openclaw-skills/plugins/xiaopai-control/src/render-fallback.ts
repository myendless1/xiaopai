import { MAX_SPEECH_LENGTH } from "./constants.js";
import type { XiaopaiCommand, XiaopaiCommandResult, XiaopaiSequenceCommand } from "./contracts.js";
import { normalizeSpeechTextForVoice } from "./speech-text.js";
import { extractCommandInput, validateXiaopaiCommand } from "./validation.js";

export const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export const XIAOPAI_EXECUTE_METHOD = "xiaopaiControl.execute";

export type XiaopaiRenderSkipReason =
  | "not_stackchan"
  | "not_xiaopai_target"
  | "no_final_text"
  | "already_rendered"
  | "fallback_disabled";

export type XiaopaiRenderOutcome =
  | "explicit_rendered"
  | "fallback_rendered"
  | "fallback_skipped"
  | "fallback_failed";

export type XiaopaiRenderContext = {
  required: true;
  device_id?: string;
  interrupt: boolean;
  event_id?: string;
  event_type?: string;
};

export type XiaopaiRenderDetection =
  | { required: true; context: XiaopaiRenderContext }
  | { required: false; reason: XiaopaiRenderSkipReason };

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

export function detectXiaopaiRenderIntentFromPrompt(prompt: unknown): XiaopaiRenderDetection {
  for (const text of extractCandidateMessageTexts(prompt)) {
    const detection = detectXiaopaiRenderIntentFromText(text);
    if (detection.required || detection.reason === "not_xiaopai_target") return detection;
    const plainTextDetection = detectPlainTextStackchanRenderIntentFromText(text);
    if (plainTextDetection.required) return plainTextDetection;
  }
  return { required: false, reason: "not_stackchan" };
}

export function detectXiaopaiRenderIntentFromMessages(messages: unknown[]): XiaopaiRenderDetection {
  let sawPlainTextStackchanPrompt = false;
  for (let index = messages.length - 1; index >= 0; index -= 1) {
    const message = messages[index];
    for (const text of extractCandidateMessageTexts(message)) {
      const detection = detectXiaopaiRenderIntentFromText(text);
      if (detection.required || detection.reason === "not_xiaopai_target") return detection;
      if (detectPlainTextStackchanRenderIntentFromText(text).required) sawPlainTextStackchanPrompt = true;
    }
  }
  if (sawPlainTextStackchanPrompt) {
    return { required: true, context: { required: true, interrupt: true, event_type: "plain_text_stackchan" } };
  }
  return { required: false, reason: "not_stackchan" };
}

export function detectXiaopaiRenderIntentFromText(text: string): XiaopaiRenderDetection {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    return { required: false, reason: "not_stackchan" };
  }
  if (!isRecord(parsed) || parsed.schema !== STACKCHAN_EVENT_SCHEMA) return { required: false, reason: "not_stackchan" };
  if (!isRecord(parsed.event)) return { required: false, reason: "not_stackchan" };
  if (!isRecord(parsed.render) || parsed.render.target !== "xiaopai") return { required: false, reason: "not_xiaopai_target" };

  const payload = isRecord(parsed.event.payload) ? parsed.event.payload : {};
  const deviceId = readTrimmedString(parsed.device_id) ?? readTrimmedString(payload.device_id);
  const eventId = readTrimmedString(parsed.event_id) ?? readTrimmedString(parsed.event.id);
  const eventType = readTrimmedString(parsed.event.type);
  const interrupt = typeof parsed.render.interrupt === "boolean" ? parsed.render.interrupt : true;
  const context: XiaopaiRenderContext = {
    required: true,
    interrupt,
    ...(deviceId ? { device_id: deviceId } : {}),
    ...(eventId ? { event_id: eventId } : {}),
    ...(eventType ? { event_type: eventType } : {})
  };
  return { required: true, context };
}

export function detectPlainTextStackchanRenderIntentFromText(text: string): XiaopaiRenderDetection {
  if (!isPlainTextStackchanSystemPrompt(text)) return { required: false, reason: "not_stackchan" };
  return { required: true, context: { required: true, interrupt: true, event_type: "plain_text_stackchan" } };
}

export function detectXiaopaiRenderIntentFromSessionKey(sessionKey: unknown): XiaopaiRenderDetection {
  if (typeof sessionKey !== "string") return { required: false, reason: "not_stackchan" };
  const normalized = sessionKey.trim().toLowerCase();
  if (!/(^|:)xiaopai[-:]/.test(normalized)) return { required: false, reason: "not_stackchan" };
  const deviceId = extractXiaopaiDeviceIdFromSessionKey(sessionKey);
  return {
    required: true,
    context: {
      required: true,
      interrupt: true,
      event_type: "plain_text_stackchan",
      ...(deviceId ? { device_id: deviceId } : {})
    }
  };
}

export function isXiaopaiExecuteTool(toolName: unknown): boolean {
  return typeof toolName === "string" && (toolName === XIAOPAI_EXECUTE_METHOD || toolName.endsWith(`.${XIAOPAI_EXECUTE_METHOD}`));
}

export function isSuccessfulXiaopaiCommandResult(result: unknown): result is XiaopaiCommandResult & { status: "queued" } {
  const payload = readResultPayload(result);
  return isRecord(payload) && payload.status === "queued";
}

export function commandContainsSpeech(input: unknown): boolean {
  const validated = validateXiaopaiCommand(extractCommandInput(input));
  if (!validated.ok) return false;
  return validatedCommandContainsSpeech(validated.value);
}

export function observeXiaopaiExecuteCall(params: unknown, result: unknown): boolean {
  return commandContainsSpeech(params) && isSuccessfulXiaopaiCommandResult(result);
}

export function observeXiaopaiExecuteCliCall(params: unknown, result: unknown): boolean {
  const command = readExecCommand(params);
  if (!command || !isXiaopaiExecuteCliCommand(command)) return false;
  const payload = extractCliParamsPayload(command);
  if (!payload || !commandContainsSpeech(payload)) return false;
  return isSuccessfulXiaopaiCommandResult(result) || isSuccessfulXiaopaiCommandResult(readCliJsonResult(result));
}

export function normalizeFallbackSpeechText(text: unknown): string {
  if (typeof text !== "string") return "";
  const withoutDiagnosticLines = text
    .replace(/\r\n/g, "\n")
    .split("\n")
    .map((line) => line.replace(/^assistant:\s*/i, "").replace(/\bMEDIA:[^\s]+/gi, "").trim())
    .filter((line) => !isDiagnosticLine(line))
    .join(" ")
    .replace(/\s+/g, " ")
    .trim();

  if (withoutDiagnosticLines === "" || isRawJsonText(withoutDiagnosticLines)) return "";
  const speechText = normalizeSpeechTextForVoice(withoutDiagnosticLines);
  if (speechText === "") return "";
  if (speechText.length <= MAX_SPEECH_LENGTH) return speechText;
  return truncateSpeechText(speechText, MAX_SPEECH_LENGTH);
}

export function buildFallbackSequenceCommand(context: XiaopaiRenderContext, speechText: string): XiaopaiSequenceCommand {
  return {
    type: "sequence",
    ...(context.device_id ? { device_id: context.device_id } : {}),
    interrupt: context.interrupt,
    steps: [
      { type: "speak", text: speechText },
      { type: "face", expression: "calm" }
    ]
  };
}

export async function executeXiaopaiRenderFallback(options: {
  state: XiaopaiRenderTurnState;
  finalText: unknown;
  execute: (input: { command: XiaopaiCommand }) => Promise<XiaopaiCommandResult>;
}): Promise<XiaopaiFallbackExecution | XiaopaiRenderDiagnostic> {
  const base = diagnosticBase(options.state.context);
  if (options.state.speechRendered) {
    return { ...base, outcome: "fallback_skipped", reason: "already_rendered" };
  }

  const speechText = normalizeFallbackSpeechText(options.finalText);
  if (speechText === "") {
    return { ...base, outcome: "fallback_skipped", reason: "no_final_text" };
  }

  const command = buildFallbackSequenceCommand(options.state.context, speechText);
  try {
    const result = await options.execute({ command });
    if (isSuccessfulXiaopaiCommandResult(result)) {
      return { command, result, diagnostic: { ...base, outcome: "fallback_rendered" } };
    }
    const reason = result.status === "rejected" ? "plugin_rejected" : "plugin_failed";
    return {
      command,
      result,
      diagnostic: {
        ...base,
        outcome: "fallback_failed",
        reason,
        details: sanitizeFailureDetails(result)
      }
    };
  } catch (error) {
    return {
      ...base,
      outcome: "fallback_failed",
      reason: "exception",
      details: sanitizeFailureDetails(error)
    };
  }
}

export function diagnosticBase(context: XiaopaiRenderContext): Omit<XiaopaiRenderDiagnostic, "outcome"> {
  return {
    ...(context.event_id ? { event_id: context.event_id } : {}),
    ...(context.device_id ? { device_id: context.device_id } : {}),
    ...(context.event_type ? { event_type: context.event_type } : {})
  };
}

export function sanitizeFailureDetails(value: unknown): Record<string, unknown> {
  if (value instanceof Error) return { error: value.message };
  if (!isRecord(value)) return { result: String(value) };
  const details: Record<string, unknown> = {};
  if (typeof value.status === "string") details.status = value.status;
  if (isRecord(value.error)) {
    details.error = {
      ...(typeof value.error.code === "string" ? { code: value.error.code } : {}),
      ...(typeof value.error.message === "string" ? { message: value.error.message } : {})
    };
  }
  if (isRecord(value.action) && isRecord(value.action.error)) {
    details.action_error = {
      ...(typeof value.action.error.code === "string" ? { code: value.action.error.code } : {}),
      ...(typeof value.action.error.message === "string" ? { message: value.action.error.message } : {})
    };
  }
  return details;
}

function extractCandidateMessageTexts(value: unknown): string[] {
  if (typeof value === "string") return [value.trim()].filter(Boolean);
  if (!isRecord(value)) return [];
  const content = value.content;
  if (typeof content === "string") return [content.trim()].filter(Boolean);
  if (!Array.isArray(content)) return [];
  const texts: string[] = [];
  for (const part of content) {
    if (typeof part === "string") {
      const trimmed = part.trim();
      if (trimmed) texts.push(trimmed);
    } else if (isRecord(part) && typeof part.text === "string") {
      const trimmed = part.text.trim();
      if (trimmed) texts.push(trimmed);
    }
  }
  return texts;
}

function validatedCommandContainsSpeech(command: XiaopaiCommand): boolean {
  if (command.type === "speak") return command.text.trim() !== "";
  if (command.type !== "sequence") return false;
  return command.steps.some((step) => step.type === "speak" && step.text.trim() !== "");
}

function readResultPayload(result: unknown): unknown {
  if (!isRecord(result)) return result;
  if ("status" in result) return result;
  if ("payload" in result) return result.payload;
  if ("result" in result) return result.result;
  return result;
}

function readExecCommand(params: unknown): string | undefined {
  if (!isRecord(params)) return undefined;
  return readTrimmedString(params.command) ?? readTrimmedString(params.cmd);
}

function isXiaopaiExecuteCliCommand(command: string): boolean {
  return /\bopenclaw\s+gateway\s+call\s+(?:tool\.)?xiaopaiControl\.execute\b/.test(command);
}

function extractCliParamsPayload(command: string): unknown {
  const raw = extractCliParamsArgument(command);
  if (!raw) return undefined;
  try {
    return JSON.parse(raw);
  } catch {
    return undefined;
  }
}

function extractCliParamsArgument(command: string): string | undefined {
  const singleQuoted = /(?:^|\s)--params\s+'([^']*)'/.exec(command);
  if (singleQuoted?.[1]) return singleQuoted[1];
  const doubleQuoted = /(?:^|\s)--params\s+"((?:\\"|[^"])*)"/.exec(command);
  if (doubleQuoted?.[1]) return doubleQuoted[1].replace(/\\"/g, '"');
  return undefined;
}

function readCliJsonResult(result: unknown): unknown {
  for (const text of extractResultTexts(result)) {
    const parsed = tryParseJson(text);
    if (parsed !== undefined) return parsed;
  }
  return undefined;
}

function extractResultTexts(value: unknown): string[] {
  if (typeof value === "string") return [value];
  if (!isRecord(value)) return [];
  const texts: string[] = [];
  pushTrimmedText(texts, value.aggregated);
  pushTrimmedText(texts, value.content);
  if (isRecord(value.details)) pushTrimmedText(texts, value.details.aggregated);
  if (Array.isArray(value.content)) {
    for (const part of value.content) {
      if (typeof part === "string") pushTrimmedText(texts, part);
      else if (isRecord(part)) pushTrimmedText(texts, part.text);
    }
  }
  return texts;
}

function pushTrimmedText(texts: string[], value: unknown): void {
  if (typeof value !== "string") return;
  const trimmed = value.trim();
  if (trimmed) texts.push(trimmed);
}

function tryParseJson(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return undefined;
  }
}

function isDiagnosticLine(line: string): boolean {
  return (
    /^```/.test(line) ||
    /^(debug|trace|diagnostic|tool result|tool call|transport):/i.test(line) ||
    /^\[(debug|trace|diagnostic|tool|transport)\]/i.test(line)
  );
}

function isRawJsonText(text: string): boolean {
  if (!/^[{[]/.test(text)) return false;
  try {
    JSON.parse(text);
    return true;
  } catch {
    return false;
  }
}

function isPlainTextStackchanSystemPrompt(text: string): boolean {
  const lower = text.toLowerCase();
  return (
    lower.includes("stack-chan") &&
    text.includes("小派") &&
    (text.includes("语音识别文本直接作为用户消息") || text.includes("非触摸设备事件")) &&
    text.includes(XIAOPAI_EXECUTE_METHOD)
  );
}

function extractXiaopaiDeviceIdFromSessionKey(sessionKey: string): string | undefined {
  const match = /(?:^|:)xiaopai-(.+)$/.exec(sessionKey.trim());
  const deviceId = match?.[1]?.trim();
  return deviceId ? deviceId.slice(0, 64) : undefined;
}

function truncateSpeechText(text: string, maxLength: number): string {
  const suffix = "...";
  const limit = maxLength - suffix.length;
  const candidate = text.slice(0, limit);
  const lastSpace = candidate.lastIndexOf(" ");
  if (lastSpace >= Math.floor(limit * 0.7)) return `${candidate.slice(0, lastSpace).trimEnd()}${suffix}`;
  return `${candidate.trimEnd()}${suffix}`;
}

function readTrimmedString(value: unknown): string | undefined {
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  return trimmed === "" ? undefined : trimmed;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
