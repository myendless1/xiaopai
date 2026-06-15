import type { XIAOPAI_ACTIONS, XIAOPAI_EXPRESSIONS, XIAOPAI_MOVE_DIRECTIONS } from "./constants.js";

export type XiaopaiExpression = (typeof XIAOPAI_EXPRESSIONS)[number];
export type XiaopaiAction = (typeof XIAOPAI_ACTIONS)[number];
export type XiaopaiMoveDirection = (typeof XIAOPAI_MOVE_DIRECTIONS)[number];

export type XiaopaiCommandBase = {
  device_id?: string;
  interrupt?: boolean;
};

export type XiaopaiSpeakCommand = XiaopaiCommandBase & {
  type: "speak";
  text: string;
};

export type XiaopaiFaceCommand = XiaopaiCommandBase & {
  type: "face";
  expression: XiaopaiExpression;
};

export type XiaopaiActionCommand = XiaopaiCommandBase & {
  type: "action";
  action: XiaopaiAction;
};

export type XiaopaiMoveCommand = XiaopaiCommandBase & {
  type: "move";
  direction: XiaopaiMoveDirection;
  degree?: number;
  duration_ms?: number;
};

export type XiaopaiSequenceStep =
  | Omit<XiaopaiSpeakCommand, "device_id" | "interrupt">
  | Omit<XiaopaiFaceCommand, "device_id" | "interrupt">
  | Omit<XiaopaiActionCommand, "device_id" | "interrupt">
  | Omit<XiaopaiMoveCommand, "device_id" | "interrupt">;

export type XiaopaiSequenceCommand = XiaopaiCommandBase & {
  type: "sequence";
  steps: XiaopaiSequenceStep[];
};

export type XiaopaiStopCommand = {
  type: "stop";
  device_id?: string;
};

export type XiaopaiCommand =
  | XiaopaiSpeakCommand
  | XiaopaiFaceCommand
  | XiaopaiActionCommand
  | XiaopaiMoveCommand
  | XiaopaiSequenceCommand
  | XiaopaiStopCommand;

export type XiaopaiValidationError = {
  code: string;
  message: string;
  field?: string;
  step_index?: number;
  value?: unknown;
};

export type XiaopaiActionRecord = {
  type: "xiaopai.command";
  status: "success" | "failed" | "skipped";
  details?: Record<string, unknown>;
  error?: {
    code: string;
    message: string;
  };
};

export type XiaopaiCommandResult = {
  status: "queued" | "rejected" | "failed";
  device_id?: string;
  action: XiaopaiActionRecord;
};

export type XiaopaiHealthResult = {
  status: "ok" | "failed";
  reachable: boolean;
  service?: string;
  details?: Record<string, unknown>;
  error?: {
    code: string;
    message: string;
  };
};

export type XiaopaiDevice = {
  device_id: string;
  online?: boolean;
  pending_commands?: number;
  last_seen_seconds_ago?: number;
  last_ack?: unknown;
};

export type XiaopaiDeviceListResult = {
  status: "ok" | "failed";
  default_device_id?: string;
  online_ttl_seconds?: number;
  devices: XiaopaiDevice[];
  error?: {
    code: string;
    message: string;
  };
};

export type ValidationResult<T> =
  | { ok: true; value: T }
  | { ok: false; error: XiaopaiValidationError };

export type XiaopaiControlConfig = {
  baseUrl: string;
  timeoutMs: number;
  dryRun: boolean;
  defaultDeviceId?: string;
};
