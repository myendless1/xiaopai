import { describe, expect, it } from "vitest";
import { CalendarIntentParser } from "../src/calendar/parser.js";
import { sampleEvent } from "./helpers.js";

describe("CalendarIntentParser", () => {
  it("parses the supported Chinese meeting creation utterance", () => {
    const intent = new CalendarIntentParser().parse(sampleEvent);
    expect(intent.title).toBe("项目会");
    expect(intent.attendeeNames).toEqual(["张三", "李四"]);
    expect(intent.start).toBe("2026-06-06T10:00:00+08:00");
    expect(intent.end).toBe("2026-06-06T11:00:00+08:00");
  });

  it("marks unsupported utterances as low confidence", () => {
    const intent = new CalendarIntentParser().parse({
      ...sampleEvent,
      payload: { text: "今天天气怎么样？" }
    });
    expect(intent.confidence).toBeLessThan(0.75);
  });
});
