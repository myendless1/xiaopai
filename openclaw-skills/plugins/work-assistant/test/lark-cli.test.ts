import { describe, expect, it } from "vitest";
import { DryRunIMAdapter } from "../src/lark/dry-run.js";
import { LarkCliCalendarAdapter, LarkCliContactAdapter, LarkCliIMAdapter, type ProcessRunner } from "../src/lark/lark-cli.js";

describe("lark-cli adapters", () => {
  it("resolves unique, ambiguous, and missing attendees from mocked process output", async () => {
    const runner: ProcessRunner = async (argv) => {
      expect(argv).toEqual([
        "contact",
        "+search-user",
        "--queries",
        "张三,李四,王五",
        "--as",
        "user",
        "--format",
        "json"
      ]);
      return {
        code: 0,
        stderr: "",
        stdout: JSON.stringify({
          data: {
            users: [
              { open_id: "ou_zhangsan", localized_name: "张三" },
              { open_id: "ou_lisi_1", localized_name: "李四", department: "研发" },
              { open_id: "ou_lisi_2", localized_name: "李四", department: "销售" }
            ]
          }
        })
      };
    };
    const result = await new LarkCliContactAdapter({ runner }).resolvePeople(["张三", "李四", "王五"]);
    expect(result["张三"]?.status).toBe("unique");
    expect(result["李四"]?.status).toBe("ambiguous");
    expect(result["王五"]?.status).toBe("missing");
  });

  it("creates calendar events through fixed argv arrays and parses event ids", async () => {
    const runner: ProcessRunner = async (argv) => {
      expect(argv).toEqual([
        "calendar",
        "+create",
        "--summary",
        "项目会",
        "--start",
        "2026-06-06T10:00:00+08:00",
        "--end",
        "2026-06-06T11:00:00+08:00",
        "--as",
        "user",
        "--format",
        "json",
        "--attendee-ids",
        "ou_zhangsan,ou_lisi"
      ]);
      return {
        code: 0,
        stderr: "",
        stdout: JSON.stringify({
          data: {
            event: {
              event_id: "evt_lark_1",
              calendar_id: "primary"
            }
          }
        })
      };
    };
    const result = await new LarkCliCalendarAdapter({ runner }).createEvent({
      title: "项目会",
      start: "2026-06-06T10:00:00+08:00",
      end: "2026-06-06T11:00:00+08:00",
      requesterId: "ou_requester",
      attendeeIds: ["ou_zhangsan", "ou_lisi"]
    });
    expect(result).toEqual({ ok: true, eventId: "evt_lark_1", calendarId: "primary", link: undefined });
  });

  it("converts process failures into stable calendar adapter failures", async () => {
    const result = await new LarkCliCalendarAdapter({
      runner: async () => ({
        code: 1,
        stdout: "",
        stderr: "permission denied"
      })
    }).createEvent({
      title: "项目会",
      start: "2026-06-06T10:00:00+08:00",
      end: "2026-06-06T11:00:00+08:00",
      requesterId: "ou_requester",
      attendeeIds: []
    });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.code).toBe("LARK_CALENDAR_CREATE_FAILED");
  });

  it("lists calendar agenda events through fixed argv arrays and parses supported shapes", async () => {
    const runner: ProcessRunner = async (argv) => {
      expect(argv).toEqual([
        "calendar",
        "+agenda",
        "--start",
        "2026-06-05T16:00:00.000Z",
        "--end",
        "2026-06-06T16:00:00.000Z",
        "--calendar-id",
        "primary",
        "--as",
        "user",
        "--format",
        "json"
      ]);
      return {
        code: 0,
        stderr: "",
        stdout: JSON.stringify({
          data: {
            items: [
              {
                event_id: "evt_2",
                summary: "下午同步",
                start_time: { date_time: "2026-06-06T14:00:00+08:00" },
                end_time: { date_time: "2026-06-06T15:00:00+08:00" },
                attendees: [{ id: "ou_1" }, { id: "ou_2" }]
              },
              {
                event: {
                  event_id: "evt_1",
                  title: "上午客户接待",
                  start: "2026-06-06T09:30:00+08:00",
                  end: "2026-06-06T10:30:00+08:00",
                  location: { name: "上海办公室" },
                  remarks: "准备材料"
                }
              }
            ]
          }
        })
      };
    };
    const result = await new LarkCliCalendarAdapter({ runner }).listEvents({
      start: "2026-06-05T16:00:00.000Z",
      end: "2026-06-06T16:00:00.000Z"
    });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.events.map((event) => event.id)).toEqual(["evt_1", "evt_2"]);
      expect(result.events[0]).toMatchObject({
        title: "上午客户接待",
        location: "上海办公室",
        description: "准备材料"
      });
      expect(result.events[1]?.attendeeCount).toBe(2);
    }
  });

  it("parses lark-cli agenda output when data is the event array", async () => {
    const result = await new LarkCliCalendarAdapter({
      runner: async () => ({
        code: 0,
        stderr: "",
        stdout: JSON.stringify({
          ok: true,
          data: [
            {
              app_link: "https://applink.feishu.cn/client/calendar/event/detail",
              event_id: "e6bc091c-a4a5-4a17-86a5-133c1faf59f2_0",
              organizer_calendar_id: "feishu.cn_gwZO8MVdRShtFeMC017gja@group.calendar.feishu.cn",
              summary: "openclaw 测试会议",
              start_time: {
                datetime: "2026-06-14T16:30:00+08:00",
                timezone: "Asia/Shanghai"
              },
              end_time: {
                datetime: "2026-06-14T17:00:00+08:00",
                timezone: "Asia/Shanghai"
              },
              description: ""
            }
          ],
          meta: {
            count: 1
          }
        })
      })
    }).listEvents({
      start: "2026-06-14T08:27:08.000Z",
      end: "2026-06-16T08:27:08.000Z"
    });

    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.events).toHaveLength(1);
      expect(result.events[0]).toMatchObject({
        id: "e6bc091c-a4a5-4a17-86a5-133c1faf59f2_0",
        title: "openclaw 测试会议",
        start: "2026-06-14T16:30:00+08:00",
        end: "2026-06-14T17:00:00+08:00",
        calendarId: "feishu.cn_gwZO8MVdRShtFeMC017gja@group.calendar.feishu.cn"
      });
    }
  });

  it("returns empty agenda results when lark-cli returns no events", async () => {
    const result = await new LarkCliCalendarAdapter({
      runner: async () => ({
        code: 0,
        stdout: JSON.stringify({ data: { items: [] } }),
        stderr: ""
      })
    }).listEvents({
      start: "2026-06-05T16:00:00.000Z",
      end: "2026-06-06T16:00:00.000Z"
    });
    expect(result).toEqual({ ok: true, calendarId: "primary", events: [] });
  });

  it("converts agenda process failures into stable list failures", async () => {
    const result = await new LarkCliCalendarAdapter({
      runner: async () => ({
        code: 1,
        stdout: "",
        stderr: "permission denied"
      })
    }).listEvents({
      start: "2026-06-05T16:00:00.000Z",
      end: "2026-06-06T16:00:00.000Z"
    });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.code).toBe("LARK_CALENDAR_LIST_FAILED");
  });

  it("converts malformed agenda JSON into stable parse failures", async () => {
    const result = await new LarkCliCalendarAdapter({
      runner: async () => ({
        code: 0,
        stdout: "{",
        stderr: ""
      })
    }).listEvents({
      start: "2026-06-05T16:00:00.000Z",
      end: "2026-06-06T16:00:00.000Z"
    });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.code).toBe("LARK_CALENDAR_LIST_PARSE_FAILED");
  });

  it("returns deterministic dry-run IM message ids without lark-cli calls", async () => {
    const adapter = new DryRunIMAdapter();
    const request = {
      text: "我会晚 5 分钟到，请大家稍等一下。",
      requesterId: "ou_requester",
      chatId: "oc_meeting_chat",
      idempotencyKey: "evt-late"
    };

    const first = await adapter.sendText(request);
    const second = await adapter.sendText(request);

    expect(first).toEqual(second);
    expect(first).toMatchObject({
      ok: true,
      chatId: "oc_meeting_chat"
    });
    expect(adapter.sentMessages).toHaveLength(2);
  });

  it("sends Lark IM messages through fixed argv arrays and parses message ids", async () => {
    const runner: ProcessRunner = async (argv) => {
      expect(argv).toEqual([
        "im",
        "+messages-send",
        "--chat-id",
        "oc_meeting_chat",
        "--text",
        "我会晚 5 分钟到，请大家稍等一下。",
        "--as",
        "user",
        "--format",
        "json",
        "--idempotency-key",
        "evt-late"
      ]);
      return {
        code: 0,
        stderr: "",
        stdout: JSON.stringify({
          data: {
            message: {
              message_id: "om_lark_1"
            }
          }
        })
      };
    };

    const result = await new LarkCliIMAdapter({ runner }).sendText({
      text: "我会晚 5 分钟到，请大家稍等一下。",
      requesterId: "ou_requester",
      chatId: "oc_meeting_chat",
      idempotencyKey: "evt-late"
    });

    expect(result).toEqual({
      ok: true,
      messageId: "om_lark_1",
      chatId: "oc_meeting_chat"
    });
  });

  it("converts Lark IM process failures and parse failures into stable results", async () => {
    const failed = await new LarkCliIMAdapter({
      runner: async () => ({
        code: 1,
        stdout: "",
        stderr: "permission denied"
      })
    }).sendText({
      text: "hello",
      requesterId: "ou_requester",
      chatId: "oc_meeting_chat"
    });

    const parseFailed = await new LarkCliIMAdapter({
      runner: async () => ({
        code: 0,
        stdout: "{",
        stderr: ""
      })
    }).sendText({
      text: "hello",
      requesterId: "ou_requester",
      attendeeUserIds: ["ou_1", "ou_2"]
    });

    expect(failed).toMatchObject({
      ok: false,
      code: "LARK_MESSAGE_SEND_FAILED"
    });
    expect(parseFailed).toMatchObject({
      ok: false,
      code: "LARK_MESSAGE_PARSE_FAILED"
    });
  });
});
