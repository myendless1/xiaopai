import { spawn } from "node:child_process";
import type { InputEvent, StructuredResponse } from "../contracts.js";
import type { ProactiveCalendarAgentDispatchConfig } from "./types.js";

export const STACKCHAN_EVENT_SCHEMA = "openclaw.stackchan.event.v1";
export const WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA = "openclaw.work_assistant.scheduler_response.v1";

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

export function buildSchedulerAgentTurnMessage(options: {
  event: InputEvent;
  response: StructuredResponse;
  config: ProactiveCalendarAgentDispatchConfig;
}): string {
  const { event, response, config } = options;
  return JSON.stringify({
    schema: STACKCHAN_EVENT_SCHEMA,
    event_id: event.event_id,
    ...(config.deviceId ? { device_id: config.deviceId } : {}),
    event: {
      event_id: event.event_id,
      type: "work_assistant_proactive_response",
      timestamp: event.timestamp,
      user_id: event.user_id,
      payload: {
        schema: WORK_ASSISTANT_SCHEDULER_RESPONSE_SCHEMA,
        source: "work_assistant_scheduler",
        text: response.speech,
        source_event: event,
        structured_response: response,
        agent_directive:
          "Use payload.structured_response as the canonical proactive reminder. Do not call workAssistant.handleEvent again for this event. Render the user-facing speech through xiaopaiControl.execute when available."
      },
      context: event.context
    },
    render: {
      target: "xiaopai",
      interrupt: config.interrupt
    }
  });
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
  if (!config.sessionKey) {
    return {
      status: "failed",
      code: "AGENT_SESSION_KEY_MISSING",
      message: "Scheduler agent dispatch is enabled but scheduler.agentDispatch.sessionKey is missing."
    };
  }

  const message = buildSchedulerAgentTurnMessage({ event, response, config });
  const scheduleParams: SchedulerAgentTurnScheduleParams = {
    sessionKey: config.sessionKey,
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
        scheduler: "session_workflow"
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
        scheduler: "openclaw_cron_cli"
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
