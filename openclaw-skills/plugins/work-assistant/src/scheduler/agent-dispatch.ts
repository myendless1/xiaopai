import { spawn } from "node:child_process";
import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { ProactiveCalendarAgentDispatchConfig } from "./types.js";

export const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export const WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA = "openclaw.work_assistant.scheduler_response.v1";
const DEFAULT_XIAOPAI_BASE_URL = "http://127.0.0.1:8091";
const DEFAULT_XIAOPAI_DEVICE_LOOKUP_TIMEOUT_MS = 2_000;

export type SchedulerAgentTurnScheduleParams = {
  sessionKey: string;
  message: string;
  delayMs: number;
  deleteAfterRun: boolean;
  deliveryMode: "none" | "announce";
  tag: string;
  name: string;
  agentId?: string;
};

export type SchedulerAgentTurnHandle = {
  id: string;
  pluginId?: string;
  sessionKey: string;
  kind: string;
};

export type SchedulerAgentTurnScheduler = (
  params: SchedulerAgentTurnScheduleParams
) => Promise<SchedulerAgentTurnHandle | undefined>;

export type SchedulerAgentDispatchRuntimeApi = {
  session?: {
    workflow?: {
      scheduleSessionTurn?: SchedulerAgentTurnScheduler;
    };
  };
  scheduleSessionTurn?: SchedulerAgentTurnScheduler;
  runAgentTurnCli?: SchedulerAgentTurnScheduler;
  resolveOnlineXiaopaiDeviceId?: (config: ProactiveCalendarAgentDispatchConfig) => Promise<string | undefined>;
  logger?: {
    info?: (message: string) => void;
    warn?: (message: string) => void;
  };
};

export type SchedulerAgentDispatchResult =
  | {
      status: "success";
      jobId: string;
      sessionKey: string;
    }
  | {
      status: "skipped";
      reason: "disabled" | "missing_speech";
    }
  | {
      status: "failed";
      code: string;
      message: string;
    };

type SchedulerAgentSessionResolution =
  | {
      ok: true;
      sessionKey: string;
      source: "static" | "online_xiaopai";
      deviceId?: string;
    }
  | {
      ok: false;
      code: string;
      message: string;
    };

export function buildSchedulerAgentTurnMessage(options: {
  event: InputEvent;
  response: StructuredResponse;
  config: ProactiveCalendarAgentDispatchConfig;
}): string {
  const { event, response, config } = options;
  const profile = selectSchedulerPromptProfile(event.type);
  const eventFacts = collectEventFacts(event, response);
  const deviceLine = config.deviceId ? `- 目标小派设备 device_id: ${config.deviceId}` : "- 目标小派设备 device_id: 未指定，调用时不要填写 device_id，让 xiaopai-control 使用默认设备。";
  const commandShape = config.deviceId
    ? `{ "type": "sequence", "device_id": "${escapeInline(config.deviceId)}", "interrupt": ${config.interrupt}, "steps": [...] }`
    : `{ "type": "sequence", "interrupt": ${config.interrupt}, "steps": [...] }`;

  return [
    "你正在处理 Work Assistant 调度器生成的被动播报任务。请直接控制小派完成播报，不要调用 workAssistant.handleEvent，也不要只返回文字。",
    "",
    "执行要求：",
    "- 必须调用 xiaopaiControl.execute 播报语音和展示表情；如果没有直连工具，就使用 OpenClaw CLI：openclaw gateway call xiaopaiControl.execute --json --params '<json>'。",
    "- 命令必须是一个 sequence，至少包含指定 face 表情、一个 speak 步骤，以及最后的 calm 表情复位；只有当下方“小派表情和动作要求”明确指定 action 时才加入 action 步骤。",
    "- speak.text 请基于下面的关键事实和 Work Assistant 已生成内容组织，保持事实准确，不要编造输入中没有的会议室、目的地、天气或人员。",
    "- speak.text 最长 500 字，适合小派直接读出，不要读出 JSON、字段名、调试信息或 Markdown。",
    "- presentation.emotion/motion/light 仅作为 Work Assistant 背景信息；实际小派表情和动作以下方“小派表情和动作要求”为准，不要自动映射额外动作。",
    "",
    "任务信息：",
    `- 任务类型: ${profile.label} (${event.type})`,
    `- 事件 ID: ${event.event_id}`,
    `- 用户 ID: ${event.user_id}`,
    `- 事件时间: ${event.timestamp}`,
    `- 时区: ${event.context.timezone}`,
    `- 打断当前播报 interrupt: ${config.interrupt}`,
    deviceLine,
    ...eventFacts.map((fact) => `- ${fact}`),
    "",
    "Work Assistant 已生成的正式结构化结果：",
    `- 原始播报: ${response.speech}`,
    `- presentation.emotion: ${response.presentation.emotion ?? "未指定"}`,
    `- presentation.motion: ${response.presentation.motion ?? "未指定"}`,
    `- presentation.light: ${response.presentation.light ?? "未指定"}`,
    `- actions: ${formatCompactJson(response.actions)}`,
    `- context_patch: ${formatCompactJson(response.context_patch)}`,
    "",
    "期望口播风格：",
    profile.speechStyle,
    "",
    "小派表情和动作要求：",
    ...profile.presentationInstructions.map((instruction) => `- ${instruction}`),
    "- 语音播报必须使用 speak 步骤，播报你按期望风格整理后的最终口播文案。",
    "- 播报结束后追加 { \"type\": \"face\", \"expression\": \"calm\" }。",
    "",
    "调用参数形态：",
    `- xiaopaiControl.execute 参数使用 { "command": ${commandShape} }。`,
    "- 不要使用 stack-chan JSON envelope；这条消息本身就是调度器给 agent 的执行提示词。",
    "",
    "请现在执行 xiaopaiControl.execute。"
  ].join("\n");
}

type SchedulerPromptProfile = {
  label: string;
  speechStyle: string;
  presentationInstructions: string[];
};

function selectSchedulerPromptProfile(eventType: string): SchedulerPromptProfile {
  switch (eventType) {
    case "daily_briefing_triggered":
      return {
        label: "定时日程管理及播报",
        speechStyle:
          "以轻快早安风格播报，例如“早上好呀！今天是x月x日（周x），新的一天开始啦！上周工作为您复盘....。建议本周....”。如果输入中没有上周复盘数据，不要编造上周事实，可自然改成“我先根据今天的日程帮您快速过一遍”。",
        presentationInstructions: [
          "微笑表情使用 xiaopai-control 支持的 face expression: happy_squint。",
          "sequence 开始先调用 { \"type\": \"face\", \"expression\": \"happy_squint\" }。"
        ]
      };
    case "meeting_starting_soon":
      return {
        label: "会议会前提醒",
        speechStyle:
          "以会前轻提醒风格播报，例如“打扰一下，xxx点 xx 会议还有 x 分钟将在 xxx 会议室开始，请提前准备参会”。如果没有会议室信息，就说“会议地点待确认”或按输入中的实际地点表达。",
        presentationInstructions: [
          "眨眼动态表情使用 xiaopai-control 支持的 face expression: smile_blink 和 action: blink。",
          "sequence 开始先调用 { \"type\": \"face\", \"expression\": \"smile_blink\" }，再调用 { \"type\": \"action\", \"action\": \"blink\" }。"
        ]
      };
    case "outdoor_event_detected":
      return {
        label: "日常外勤出行规划",
        speechStyle:
          "以亲切外勤提醒风格播报，例如“哈喽，您下午 xxx 点有外出行程，目的地是xxxxx，请提前预留时间哦”。如果实际不是下午，请按输入中的真实时间段表达。",
        presentationInstructions: [
          "微笑表情使用 xiaopai-control 支持的 face expression: happy_squint。",
          "sequence 开始先调用 { \"type\": \"face\", \"expression\": \"happy_squint\" }。"
        ]
      };
    case "business_trip_tomorrow_detected":
      return {
        label: "异地出差出行建议",
        speechStyle:
          "以下班前轻松提醒风格播报，例如“哈喽，准备下班咯~提醒您明早 x 点将前往 xx 出差，xx 明日天气...，与深圳温差较大哦~”。如果输入中没有深圳天气或温差数据，不要断言温差较大，可说“也留意和深圳的体感差异哦”。",
        presentationInstructions: [
          "微笑表情使用 xiaopai-control 支持的 face expression: happy_squint。",
          "sequence 开始先调用 { \"type\": \"face\", \"expression\": \"happy_squint\" }。"
        ]
      };
    default:
      return {
        label: "Work Assistant 被动播报",
        speechStyle: "以简短、自然、适合小派播报的中文播报 Work Assistant 已生成的正式结果。",
        presentationInstructions: [
          "默认表情使用 xiaopai-control 支持的 face expression: calm。",
          "sequence 开始先调用 { \"type\": \"face\", \"expression\": \"calm\" }。"
        ]
      };
  }
}

function collectEventFacts(event: InputEvent, response: StructuredResponse): string[] {
  const facts: string[] = [];
  const trigger = isRecord(event.payload.trigger) ? event.payload.trigger : undefined;
  if (trigger) {
    pushFact(facts, "调度规则", trigger.rule_id);
    pushFact(facts, "计划触发时间", trigger.scheduled_for);
    pushFact(facts, "实际触发时间", trigger.fired_at);
    pushFact(facts, "触发键", trigger.trigger_key);
    pushFact(facts, "日历 ID", trigger.calendar_id);
    pushFact(facts, "来源日程 ID", trigger.source_event_id);
  }

  const calendarEvent = isRecord(event.payload.calendar_event) ? event.payload.calendar_event : undefined;
  if (calendarEvent) {
    pushFact(facts, "日程标题", calendarEvent.title);
    pushFact(facts, "日程开始", calendarEvent.start);
    pushFact(facts, "日程结束", calendarEvent.end);
    pushFact(facts, "日程地点", calendarEvent.location);
    pushFact(facts, "日程描述", calendarEvent.description);
  }

  const minutesUntilStart = calculateMinutesUntilStart(trigger?.fired_at, calendarEvent?.start);
  if (minutesUntilStart !== undefined) facts.push(`距离日程开始: ${minutesUntilStart} 分钟`);

  const travelSummary = isRecord(response.context_patch.travel_summary) ? response.context_patch.travel_summary : undefined;
  if (travelSummary) {
    pushFact(facts, "目的地", travelSummary.destination);
    pushFact(facts, "出行日期", travelSummary.trip_date);
    pushFact(facts, "天气摘要", travelSummary.weather_status);
  }

  for (const action of response.actions) {
    if (!isRecord(action.details)) continue;
    pushFact(facts, `${action.type} 目的地`, action.details.destination);
    pushFact(facts, `${action.type} 推荐出发时间`, action.details.recommended_departure_time);
    pushFact(facts, `${action.type} 天气摘要`, action.details.summary);
    pushFact(facts, `${action.type} 日期`, action.details.date);
  }

  if (facts.length === 0) facts.push("无额外结构化事实，请以 Work Assistant 原始播报为准");
  return [...new Set(facts)];
}

function pushFact(facts: string[], label: string, value: unknown): void {
  if (typeof value === "string" && value.trim() !== "") facts.push(`${label}: ${value.trim()}`);
  else if (typeof value === "number" && Number.isFinite(value)) facts.push(`${label}: ${value}`);
}

function calculateMinutesUntilStart(firedAt: unknown, startsAt: unknown): number | undefined {
  if (typeof firedAt !== "string" || typeof startsAt !== "string") return undefined;
  const fired = Date.parse(firedAt);
  const start = Date.parse(startsAt);
  if (!Number.isFinite(fired) || !Number.isFinite(start)) return undefined;
  return Math.max(0, Math.round((start - fired) / 60_000));
}

function formatCompactJson(value: unknown): string {
  const json = JSON.stringify(value);
  if (!json) return "null";
  return json.length <= 1000 ? json : `${json.slice(0, 997)}...`;
}

function escapeInline(value: string): string {
  return value.replaceAll("\\", "\\\\").replaceAll("\"", "\\\"");
}

export async function dispatchSchedulerResponseToAgent(options: {
  api: SchedulerAgentDispatchRuntimeApi;
  event: InputEvent;
  response: StructuredResponse;
  config: ProactiveCalendarAgentDispatchConfig;
}): Promise<SchedulerAgentDispatchResult> {
  const { api, event, response, config } = options;
  if (!config.enabled) return { status: "skipped", reason: "disabled" };
  if (response.speech.trim() === "") return { status: "skipped", reason: "missing_speech" };
  const sessionResolution = await resolveSchedulerAgentSession({ api, config });
  if (!sessionResolution.ok) return { status: "failed", code: sessionResolution.code, message: sessionResolution.message };

  const effectiveConfig =
    sessionResolution.deviceId && !config.deviceId
      ? { ...config, deviceId: sessionResolution.deviceId }
      : config;
  const message = buildSchedulerAgentTurnMessage({ event, response, config: effectiveConfig });
  const scheduleParams: SchedulerAgentTurnScheduleParams = {
    sessionKey: sessionResolution.sessionKey,
    message,
    delayMs: 0,
    deleteAfterRun: true,
    deliveryMode: config.deliveryMode,
    tag: "work-assistant-scheduler",
    name: schedulerTurnName(event.event_id),
    ...(config.agentId ? { agentId: config.agentId } : {})
  };
  const scheduleSessionTurn = api.session?.workflow?.scheduleSessionTurn ?? api.scheduleSessionTurn;
  const handle = scheduleSessionTurn ? await tryScheduleSessionTurn(scheduleSessionTurn, scheduleParams) : undefined;
  if (handle?.id) {
    api.logger?.info?.(
      `work-assistant scheduler queued agent turn ${JSON.stringify({
        event_id: event.event_id,
        sessionKey: handle.sessionKey,
        jobId: handle.id,
        scheduler: "session_workflow",
        sessionKeySource: sessionResolution.source,
        ...(sessionResolution.deviceId ? { deviceId: sessionResolution.deviceId } : {})
      })}`
    );
    return {
      status: "success",
      jobId: handle.id,
      sessionKey: handle.sessionKey
    };
  }

  const cliScheduler = api.runAgentTurnCli ?? scheduleAgentTurnWithOpenClawCron;
  try {
    const cliHandle = await cliScheduler(scheduleParams);
    if (!cliHandle?.id) {
      return {
        status: "failed",
        code: "AGENT_TURN_NOT_SCHEDULED",
        message: "OpenClaw did not accept the scheduler-produced agent turn."
      };
    }
    api.logger?.info?.(
      `work-assistant scheduler queued agent turn ${JSON.stringify({
        event_id: event.event_id,
        sessionKey: cliHandle.sessionKey,
        jobId: cliHandle.id,
        scheduler: "openclaw_cron_cli",
        sessionKeySource: sessionResolution.source,
        ...(sessionResolution.deviceId ? { deviceId: sessionResolution.deviceId } : {})
      })}`
    );
    return {
      status: "success",
      jobId: cliHandle.id,
      sessionKey: cliHandle.sessionKey
    };
  } catch (error) {
    const messageText = error instanceof Error ? error.message : String(error);
    return {
      status: "failed",
      code: "AGENT_TURN_SCHEDULE_FAILED",
      message: messageText
    };
  }
}

async function resolveSchedulerAgentSession(options: {
  api: SchedulerAgentDispatchRuntimeApi;
  config: ProactiveCalendarAgentDispatchConfig;
}): Promise<SchedulerAgentSessionResolution> {
  const { api, config } = options;
  if ((config.sessionKeyMode ?? "static") !== "online_xiaopai") {
    if (!config.sessionKey) {
      return {
        ok: false,
        code: "AGENT_SESSION_KEY_MISSING",
        message: "Scheduler agent dispatch is enabled but scheduler.agentDispatch.sessionKey is missing."
      };
    }
    return { ok: true, sessionKey: config.sessionKey, source: "static" };
  }

  let deviceId: string | undefined;
  try {
    deviceId = api.resolveOnlineXiaopaiDeviceId
      ? await api.resolveOnlineXiaopaiDeviceId(config)
      : await resolveOnlineXiaopaiDeviceIdFromStackChan(config);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return {
      ok: false,
      code: "AGENT_SESSION_KEY_DEVICE_LOOKUP_FAILED",
      message: `Unable to resolve an online Xiaopai device for scheduler.agentDispatch.sessionKeyMode=online_xiaopai: ${message}`
    };
  }
  if (!deviceId) {
    return {
      ok: false,
      code: "AGENT_SESSION_KEY_DEVICE_MISSING",
      message: "Unable to resolve an online Xiaopai device for scheduler.agentDispatch.sessionKeyMode=online_xiaopai."
    };
  }
  return {
    ok: true,
    sessionKey: buildDynamicXiaopaiSessionKey(config, deviceId),
    source: "online_xiaopai",
    deviceId
  };
}

async function resolveOnlineXiaopaiDeviceIdFromStackChan(
  config: ProactiveCalendarAgentDispatchConfig
): Promise<string | undefined> {
  const baseUrl = (config.xiaopaiBaseUrl ?? process.env.STACKCHAN_BASE_URL ?? DEFAULT_XIAOPAI_BASE_URL).replace(/\/+$/, "");
  const timeoutMs = config.xiaopaiDeviceLookupTimeoutMs ?? DEFAULT_XIAOPAI_DEVICE_LOOKUP_TIMEOUT_MS;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`${baseUrl}/devices`, { signal: controller.signal });
    const text = await response.text();
    if (!response.ok) throw new Error(`stack-chan /devices returned HTTP ${response.status}`);
    const parsed = parseJson(text);
    if (parsed === undefined) throw new Error("stack-chan /devices returned malformed JSON");
    return selectOnlineXiaopaiDeviceId(parsed);
  } finally {
    clearTimeout(timer);
  }
}

export function buildDynamicXiaopaiSessionKey(
  config: Pick<ProactiveCalendarAgentDispatchConfig, "sessionKey" | "agentId">,
  deviceId: string
): string {
  const safeDeviceId = safeOpenClawSessionPart(deviceId);
  if (config.sessionKey?.includes("{device_id}")) {
    return config.sessionKey.replaceAll("{device_id}", safeDeviceId);
  }
  const marker = "xiaopai-";
  const markerIndex = config.sessionKey?.lastIndexOf(marker) ?? -1;
  if (config.sessionKey && markerIndex >= 0) {
    return `${config.sessionKey.slice(0, markerIndex + marker.length)}${safeDeviceId}`;
  }
  const agentId = safeOpenClawSessionPart(config.agentId ?? "main");
  return `agent:${agentId}:xiaopai-${safeDeviceId}`;
}

export function selectOnlineXiaopaiDeviceId(value: unknown): string | undefined {
  if (!isRecord(value)) return undefined;
  const realtimeDeviceId = selectDeviceId(value.realtime_devices, true);
  if (realtimeDeviceId) return realtimeDeviceId;
  const onlineDeviceId = selectDeviceId(value.devices, true);
  if (onlineDeviceId) return onlineDeviceId;
  const defaultDeviceId = readDeviceId(value.default_device_id);
  return defaultDeviceId && defaultDeviceId !== "default" ? defaultDeviceId : undefined;
}

function selectDeviceId(value: unknown, requireOnline: boolean): string | undefined {
  if (!Array.isArray(value)) return undefined;
  for (const item of value) {
    if (!isRecord(item)) continue;
    if (requireOnline && item.online !== true) continue;
    const deviceId = readDeviceId(item.device_id);
    if (deviceId) return deviceId;
  }
  return undefined;
}

function readDeviceId(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : undefined;
}

function safeOpenClawSessionPart(value: string): string {
  const safe = value.trim().replace(/[^A-Za-z0-9_.:-]+/g, "_").slice(0, 64);
  return safe || "default";
}

function parseJson(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return undefined;
  }
}

async function tryScheduleSessionTurn(
  scheduleSessionTurn: SchedulerAgentTurnScheduler,
  params: SchedulerAgentTurnScheduleParams
): Promise<SchedulerAgentTurnHandle | undefined> {
  try {
    return await scheduleSessionTurn(params);
  } catch {
    return undefined;
  }
}

export async function scheduleAgentTurnWithOpenClawCron(
  params: SchedulerAgentTurnScheduleParams
): Promise<SchedulerAgentTurnHandle | undefined> {
  const args = buildOpenClawCronAddArgs(params);
  const result = await runOpenClawCli(args);
  const parsed = JSON.parse(result.stdout) as unknown;
  const job = readCronJobPayload(parsed);
  if (!job.id) return undefined;
  return {
    id: job.id,
    pluginId: "work-assistant",
    sessionKey: job.sessionKey ?? params.sessionKey,
    kind: "cron-agent-turn"
  };
}

export function buildOpenClawCronAddArgs(params: SchedulerAgentTurnScheduleParams): string[] {
  const args = [
    "cron",
    "add",
    "--json",
    "--at",
    "1s",
    "--wake",
    "now",
    "--delete-after-run",
    "--session-key",
    params.sessionKey,
    "--session",
    "isolated",
    "--message",
    params.message,
    "--name",
    params.name,
    "--description",
    params.tag,
    "--light-context",
    "--thinking",
    "low",
    "--no-deliver"
  ];
  if (params.agentId) args.push("--agent", params.agentId);
  if (params.deliveryMode === "announce") args.push("--announce");
  return args;
}

function runOpenClawCli(args: string[]): Promise<{ stdout: string; stderr: string }> {
  return new Promise((resolve, reject) => {
    const child = spawn(process.env.OPENCLAW_CLI_PATH ?? "openclaw", args, {
      stdio: ["ignore", "pipe", "pipe"],
      env: process.env
    });
    const stdout: Buffer[] = [];
    const stderr: Buffer[] = [];
    child.stdout.on("data", (chunk) => stdout.push(Buffer.from(chunk)));
    child.stderr.on("data", (chunk) => stderr.push(Buffer.from(chunk)));
    child.on("error", reject);
    child.on("close", (code) => {
      const out = Buffer.concat(stdout).toString("utf8");
      const err = Buffer.concat(stderr).toString("utf8");
      if (code === 0) {
        resolve({ stdout: out, stderr: err });
      } else {
        reject(new Error(err.trim() || `openclaw exited with code ${code ?? "unknown"}`));
      }
    });
  });
}

function readCronJobPayload(value: unknown): { id?: string; sessionKey?: string } {
  if (!isRecord(value)) return {};
  const job = isRecord(value.job) ? value.job : value;
  return {
    ...(typeof job.id === "string" ? { id: job.id } : {}),
    ...(typeof job.sessionKey === "string" ? { sessionKey: job.sessionKey } : {})
  };
}

function schedulerTurnName(eventId: string): string {
  const normalized = eventId.replace(/[^A-Za-z0-9_.-]+/g, "_").slice(0, 80);
  return normalized ? `scheduler_${normalized}` : "scheduler_event";
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
