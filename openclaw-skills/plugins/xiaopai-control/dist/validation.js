import { MAX_MOVE_DEGREE, MAX_MOVE_DURATION_MS, MAX_SEQUENCE_STEPS, MAX_SPEECH_LENGTH, MIN_MOVE_DEGREE, MIN_MOVE_DURATION_MS, XIAOPAI_ACTIONS, XIAOPAI_EXPRESSIONS, XIAOPAI_MOVE_DIRECTIONS } from "./constants.js";
import { rejectedCommandResult } from "./results.js";
const expressions = new Set(XIAOPAI_EXPRESSIONS);
const actions = new Set(XIAOPAI_ACTIONS);
const directions = new Set(XIAOPAI_MOVE_DIRECTIONS);
export function extractCommandInput(value) {
    if (isRecord(value) && "command" in value)
        return value.command;
    return value;
}
export function validateXiaopaiCommand(value) {
    const command = validateCommandRecord(value, undefined);
    if (!command.ok)
        return command;
    return { ok: true, value: command.value };
}
function validateCommandRecord(value, stepIndex) {
    if (!isRecord(value))
        return invalid("invalid_command", "Command must be an object.", "command", stepIndex);
    if (typeof value.type !== "string" || value.type.trim() === "") {
        return invalid("missing_type", "Command type must be a non-empty string.", "type", stepIndex, value.type);
    }
    switch (value.type) {
        case "speak":
            return validateSpeak(value, stepIndex);
        case "face":
            return validateFace(value, stepIndex);
        case "action":
            return validateAction(value, stepIndex);
        case "move":
            return validateMove(value, stepIndex);
        case "sequence":
            if (stepIndex !== undefined)
                return invalid("nested_sequence_unsupported", "Sequence steps cannot be nested sequences.", "type", stepIndex, value.type);
            return validateSequence(value);
        case "stop":
            if (stepIndex !== undefined)
                return invalid("sequence_stop_unsupported", "Sequence steps cannot include stop.", "type", stepIndex, value.type);
            return validateStop(value);
        default:
            return invalid("unsupported_command_type", `Unsupported Xiaopai command type: ${value.type}.`, "type", stepIndex, value.type);
    }
}
function validateSpeak(value, stepIndex) {
    const text = value.text;
    if (typeof text !== "string" || text.trim() === "") {
        return invalid("invalid_speech_text", "speak.text must be a non-empty string.", "text", stepIndex, text);
    }
    const trimmed = text.trim();
    if (trimmed.length > MAX_SPEECH_LENGTH) {
        return invalid("speech_text_too_long", `speak.text must be ${MAX_SPEECH_LENGTH} characters or fewer.`, "text", stepIndex);
    }
    const base = validateBase(value, stepIndex);
    if (!base.ok)
        return base;
    return { ok: true, value: { type: "speak", text: trimmed, ...base.value } };
}
function validateFace(value, stepIndex) {
    if (typeof value.expression !== "string" || !expressions.has(value.expression)) {
        return invalid("unsupported_expression", `Unsupported Xiaopai expression: ${String(value.expression)}.`, "expression", stepIndex, value.expression);
    }
    const base = validateBase(value, stepIndex);
    if (!base.ok)
        return base;
    return { ok: true, value: { type: "face", expression: value.expression, ...base.value } };
}
function validateAction(value, stepIndex) {
    if (typeof value.action !== "string" || !actions.has(value.action)) {
        return invalid("unsupported_action", `Unsupported Xiaopai action: ${String(value.action)}.`, "action", stepIndex, value.action);
    }
    const base = validateBase(value, stepIndex);
    if (!base.ok)
        return base;
    return { ok: true, value: { type: "action", action: value.action, ...base.value } };
}
function validateMove(value, stepIndex) {
    const direction = value.direction ?? value.action;
    if (typeof direction !== "string" || !directions.has(direction)) {
        return invalid("unsupported_move_direction", `Unsupported Xiaopai move direction: ${String(direction)}.`, "direction", stepIndex, direction);
    }
    const degree = readOptionalBoundedNumber(value.degree, MIN_MOVE_DEGREE, MAX_MOVE_DEGREE);
    if (!degree.ok)
        return invalid("invalid_move_degree", `move.degree must be between ${MIN_MOVE_DEGREE} and ${MAX_MOVE_DEGREE}.`, "degree", stepIndex, value.degree);
    const duration = readOptionalBoundedNumber(value.duration_ms, MIN_MOVE_DURATION_MS, MAX_MOVE_DURATION_MS);
    if (!duration.ok) {
        return invalid("invalid_move_duration", `move.duration_ms must be between ${MIN_MOVE_DURATION_MS} and ${MAX_MOVE_DURATION_MS}.`, "duration_ms", stepIndex, value.duration_ms);
    }
    const base = validateBase(value, stepIndex);
    if (!base.ok)
        return base;
    return {
        ok: true,
        value: {
            type: "move",
            direction: direction,
            ...(degree.value === undefined ? {} : { degree: degree.value }),
            ...(duration.value === undefined ? {} : { duration_ms: duration.value }),
            ...base.value
        }
    };
}
function validateSequence(value) {
    if (!Array.isArray(value.steps) || value.steps.length === 0) {
        return invalid("invalid_sequence_steps", "sequence.steps must be a non-empty array.", "steps");
    }
    if (value.steps.length > MAX_SEQUENCE_STEPS) {
        return invalid("sequence_too_long", `sequence.steps must contain ${MAX_SEQUENCE_STEPS} steps or fewer.`, "steps");
    }
    const base = validateBase(value, undefined);
    if (!base.ok)
        return base;
    const steps = [];
    for (let index = 0; index < value.steps.length; index += 1) {
        const step = validateCommandRecord(value.steps[index], index);
        if (!step.ok)
            return step;
        steps.push(stripTopLevelFields(step.value));
    }
    return { ok: true, value: { type: "sequence", steps, ...base.value } };
}
function validateStop(value) {
    const base = validateBase(value, undefined);
    if (!base.ok)
        return base;
    const { interrupt: _interrupt, ...rest } = base.value;
    return { ok: true, value: { type: "stop", ...rest } };
}
function validateBase(value, stepIndex) {
    if (stepIndex !== undefined)
        return { ok: true, value: {} };
    const result = {};
    if ("device_id" in value) {
        if (typeof value.device_id !== "string" || value.device_id.trim() === "") {
            return invalid("invalid_device_id", "device_id must be a non-empty string when provided.", "device_id", stepIndex, value.device_id);
        }
        result.device_id = value.device_id.trim();
    }
    if ("interrupt" in value) {
        if (typeof value.interrupt !== "boolean") {
            return invalid("invalid_interrupt", "interrupt must be a boolean when provided.", "interrupt", stepIndex, value.interrupt);
        }
        result.interrupt = value.interrupt;
    }
    return { ok: true, value: result };
}
function stripTopLevelFields(command) {
    const { device_id: _deviceId, interrupt: _interrupt, ...step } = command;
    return step;
}
function readOptionalBoundedNumber(value, min, max) {
    if (value === undefined)
        return { ok: true, value: undefined };
    if (typeof value !== "number" || !Number.isFinite(value) || value < min || value > max)
        return { ok: false };
    return { ok: true, value };
}
function invalid(code, message, field, stepIndex, value) {
    const error = {
        code,
        message,
        ...(field ? { field } : {}),
        ...(stepIndex === undefined ? {} : { step_index: stepIndex }),
        ...(value === undefined ? {} : { value })
    };
    return { ok: false, error };
}
export function rejectedResultForValidation(error) {
    return rejectedCommandResult(error);
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
//# sourceMappingURL=validation.js.map