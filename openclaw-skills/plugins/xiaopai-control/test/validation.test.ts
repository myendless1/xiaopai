import { describe, expect, it } from "vitest";
import { validateXiaopaiCommand } from "../src/validation.js";

describe("validateXiaopaiCommand", () => {
  it("rejects missing or empty speech text", () => {
    expect(validateXiaopaiCommand({ type: "speak" })).toMatchObject({
      ok: false,
      error: { code: "invalid_speech_text", field: "text" }
    });
    expect(validateXiaopaiCommand({ type: "speak", text: "  " })).toMatchObject({
      ok: false,
      error: { code: "invalid_speech_text", field: "text" }
    });
  });

  it("rejects unsupported expressions and actions", () => {
    expect(validateXiaopaiCommand({ type: "face", expression: "confused" })).toMatchObject({
      ok: false,
      error: { code: "unsupported_expression", field: "expression", value: "confused" }
    });
    expect(validateXiaopaiCommand({ type: "action", action: "dance" })).toMatchObject({
      ok: false,
      error: { code: "unsupported_action", field: "action", value: "dance" }
    });
  });

  it("rejects invalid movement direction and numeric bounds", () => {
    expect(validateXiaopaiCommand({ type: "move", direction: "around" })).toMatchObject({
      ok: false,
      error: { code: "unsupported_move_direction", field: "direction" }
    });
    expect(validateXiaopaiCommand({ type: "move", direction: "left", degree: 90 })).toMatchObject({
      ok: false,
      error: { code: "invalid_move_degree", field: "degree" }
    });
    expect(validateXiaopaiCommand({ type: "move", direction: "left", duration_ms: 10 })).toMatchObject({
      ok: false,
      error: { code: "invalid_move_duration", field: "duration_ms" }
    });
  });

  it("accepts center movement without degree", () => {
    expect(validateXiaopaiCommand({ type: "move", direction: "center" })).toEqual({
      ok: true,
      value: { type: "move", direction: "center" }
    });
  });

  it("reports invalid sequence step indexes", () => {
    expect(
      validateXiaopaiCommand({
        type: "sequence",
        steps: [
          { type: "face", expression: "thinking" },
          { type: "move", direction: "left", degree: 99 }
        ]
      })
    ).toMatchObject({
      ok: false,
      error: { code: "invalid_move_degree", field: "degree", step_index: 1 }
    });
  });
});
