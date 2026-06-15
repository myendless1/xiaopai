#!/usr/bin/env node
import {
  AgendaBriefingAssistant,
  CalendarAssistant,
  JsonFileTriggerPlanStore,
  MeetingReminderAssistant,
  MemoryIdempotencyStore,
  MemoryTriggerPlanStore,
  ProactiveCalendarTriggerScheduler,
  TravelPlannerAssistant,
  WellbeingCompanionAssistant,
  createWorkAssistantHandler,
  readSchedulerConfig
} from "../dist/index.js";
import { DryRunCalendarAdapter, DryRunContactAdapter, DryRunIMAdapter } from "../dist/lark/dry-run.js";
import { LarkCliCalendarAdapter, LarkCliContactAdapter, LarkCliIMAdapter } from "../dist/lark/lark-cli.js";
import {
  ConfiguredUserProfileAdapter,
  DryRunRouteAdapter,
  DryRunWeatherAdapter,
  UnavailableRouteAdapter,
  UnavailableWeatherAdapter,
  createDryRunUserProfileAdapter
} from "../dist/travel/dry-run.js";

const args = process.argv.slice(2);
if (hasFlag("--help") || hasFlag("-h")) {
  printHelp();
  process.exit(0);
}

const timezone = readStringOption("--timezone", "WORK_ASSISTANT_TIMEZONE", "Asia/Shanghai");
const dryRun = readBooleanOption("--dry-run", "WORK_ASSISTANT_DRY_RUN", false);
const once = hasFlag("--once");
const dispatch = !hasFlag("--no-dispatch");
const intervalMs = readNumberOption("--interval-ms", "WORK_ASSISTANT_SCAN_INTERVAL_MS", 30_000);
const lookaheadHours = readNumberOption("--lookahead-hours", "WORK_ASSISTANT_LOOKAHEAD_HOURS", 48);
const meetingOffsetMinutes = readNumberOption("--meeting-offset-minutes", "WORK_ASSISTANT_MEETING_OFFSET_MINUTES", 2);
const outdoorOffsetMinutes = readNumberOption("--outdoor-offset-minutes", "WORK_ASSISTANT_OUTDOOR_OFFSET_MINUTES", 2);
const userId = readStringOption("--user-id", "WORK_ASSISTANT_USER_ID", "ou_requester");
const calendarId = readStringOption("--calendar-id", "WORK_ASSISTANT_CALENDAR_ID", "primary");
const larkCliPath = readStringOption(
  "--lark-cli-path",
  "LARK_CLI_PATH",
  "/home/ubuntu/openclaw-skills/plugins/work-assistant/scripts/lark-cli-user"
);
const larkIdentity = readStringOption("--lark-identity", "LARK_IDENTITY", "user");
const statePath = readOptionalStringOption("--state-path", "WORK_ASSISTANT_STATE_PATH");
const dailyBriefingTime = readStringOption("--daily-briefing-time", "WORK_ASSISTANT_DAILY_BRIEFING_TIME", currentLocalTime(timezone));
const businessTripTime = readStringOption("--business-trip-time", "WORK_ASSISTANT_BUSINESS_TRIP_TIME", currentLocalTime(timezone));
const originAddress = readStringOption("--origin-address", "WORK_ASSISTANT_ORIGIN_ADDRESS", "上海办公室");

const calendarAdapter = dryRun
  ? new DryRunCalendarAdapter()
  : new LarkCliCalendarAdapter({
      cliPath: larkCliPath,
      identity: larkIdentity === "bot" ? "bot" : "user"
    });
const contactAdapter = dryRun
  ? new DryRunContactAdapter()
  : new LarkCliContactAdapter({
      cliPath: larkCliPath,
      identity: larkIdentity === "bot" ? "bot" : "user"
    });
const imAdapter = dryRun
  ? new DryRunIMAdapter()
  : new LarkCliIMAdapter({
      cliPath: larkCliPath,
      identity: larkIdentity === "bot" ? "bot" : "user"
    });
const routeAdapter = dryRun ? new DryRunRouteAdapter() : new UnavailableRouteAdapter();
const weatherAdapter = dryRun ? new DryRunWeatherAdapter() : new UnavailableWeatherAdapter();
const travelConfig = {
  originAddress,
  defaultRouteMode: "driving",
  arrivalBufferMinutes: 15
};
const userProfileAdapter = dryRun
  ? createDryRunUserProfileAdapter(travelConfig)
  : new ConfiguredUserProfileAdapter(travelConfig);

const assistant = createWorkAssistantHandler({
  calendarAssistant: new CalendarAssistant({ contactAdapter, calendarAdapter }),
  agendaBriefingAssistant: new AgendaBriefingAssistant({ calendarAdapter }),
  meetingReminderAssistant: new MeetingReminderAssistant({ imAdapter }),
  travelPlannerAssistant: new TravelPlannerAssistant({
    routeAdapter,
    weatherAdapter,
    userProfileAdapter,
    ...travelConfig
  }),
  wellbeingCompanionAssistant: new WellbeingCompanionAssistant({ calendarAdapter }),
  idempotencyStore: new MemoryIdempotencyStore()
});

const config = readSchedulerConfig({
  enabled: true,
  startIntervalLoop: false,
  scanIntervalMs: intervalMs,
  lookaheadHours,
  timezone,
  userId,
  calendarId,
  maxDispatchAttempts: 3,
  rules: {
    dailyBriefing: {
      enabled: true,
      localTime: dailyBriefingTime
    },
    meetingStartingSoon: {
      enabled: true,
      offsetMinutes: meetingOffsetMinutes
    },
    outdoorEvent: {
      enabled: true,
      offsetMinutes: outdoorOffsetMinutes
    },
    businessTripTomorrow: {
      enabled: true,
      localTime: businessTripTime
    }
  }
});
const store = statePath ? new JsonFileTriggerPlanStore(statePath) : new MemoryTriggerPlanStore();
const scheduler = new ProactiveCalendarTriggerScheduler({
  config,
  calendarAdapter,
  store,
  dispatch: (event) => assistant.handleEvent(event)
});

const seenRecords = new Map();
let stopped = false;

process.on("SIGINT", () => {
  stopped = true;
  console.log("\n[watch] stopped");
});

console.log("[watch] work-assistant scheduler monitor started");
console.log(
  `[watch] dryRun=${dryRun} dispatch=${dispatch} intervalMs=${intervalMs} timezone=${timezone} calendarId=${calendarId}`
);
console.log(
  `[watch] rules: dailyBriefing=${dailyBriefingTime}, meetingOffset=${meetingOffsetMinutes}m, outdoorOffset=${outdoorOffsetMinutes}m, businessTrip=${businessTripTime}`
);
if (!dryRun) {
  console.log(`[watch] larkCliPath=${larkCliPath} larkIdentity=${larkIdentity}`);
}
if (statePath) console.log(`[watch] statePath=${statePath}`);

await runTick();
while (!once && !stopped) {
  await sleep(intervalMs);
  if (!stopped) await runTick();
}

async function runTick() {
  const now = new Date();
  console.log(`\n[${formatInstant(now.toISOString(), timezone)}] scanning calendar...`);
  const scan = await scheduler.refresh(now);
  if (!scan.ok) {
    console.log(`[scan] failed calendarId=${scan.calendarId} code=${scan.code} message=${scan.message}`);
    return;
  }

  console.log(
    `[scan] ok calendarId=${scan.calendarId} events=${scan.eventCount} plans=${scan.planCount} upserted=${scan.upserted} replaced=${scan.replacedPending}`
  );

  const records = await scheduler.listRecords();
  const changed = records.filter((record) => {
    const fingerprint = `${record.status}:${record.updatedAt}:${record.attempts}:${record.lastDispatchError ?? ""}`;
    if (seenRecords.get(record.key) === fingerprint) return false;
    seenRecords.set(record.key, fingerprint);
    return true;
  });

  for (const record of changed) printRecord(record, timezone);
  if (changed.length === 0) console.log("[plans] no new or changed trigger plans");

  if (!dispatch) return;
  const dispatches = await scheduler.dispatchDue(now);
  if (dispatches.length === 0) {
    console.log("[dispatch] no due trigger plans");
    return;
  }
  const afterDispatch = new Map((await scheduler.listRecords()).map((record) => [record.key, record]));
  for (const item of dispatches) {
    const record = afterDispatch.get(item.key);
    console.log(`[dispatch] ${item.status} type=${item.type} eventId=${item.eventId}${item.error ? ` error=${item.error}` : ""}`);
    if (record?.responseSummary) {
      console.log(`  speech: ${record.responseSummary.speech || "(empty)"}`);
      console.log(
        `  response: actions=${record.responseSummary.actionCount} followUp=${record.responseSummary.followUpExpected}`
      );
    }
  }
}

function printRecord(record, timeZone) {
  const event = record.calendarEvent;
  const title = event?.title ?? "(time-based trigger)";
  const start = event?.start ? formatInstant(event.start, timeZone) : "-";
  const location = event?.location ? ` location=${event.location}` : "";
  const desc = event?.description ? ` description=${truncate(event.description, 80)}` : "";
  console.log(
    `[plan] ${record.status} type=${record.type} due=${formatInstant(record.scheduledFor, timeZone)} title=${title} start=${start}${location}${desc}`
  );
  if (record.lastDispatchError) console.log(`  lastDispatchError: ${record.lastDispatchError}`);
}

function readOptionalStringOption(flag, envName) {
  const value = readArgValue(flag) ?? process.env[envName];
  return value && value.trim() !== "" ? value.trim() : undefined;
}

function readStringOption(flag, envName, fallback) {
  return readOptionalStringOption(flag, envName) ?? fallback;
}

function readBooleanOption(flag, envName, fallback) {
  if (hasFlag(flag)) return true;
  const raw = process.env[envName];
  if (raw === undefined) return fallback;
  return /^(1|true|yes|on)$/i.test(raw);
}

function readNumberOption(flag, envName, fallback) {
  const raw = readArgValue(flag) ?? process.env[envName];
  if (raw === undefined) return fallback;
  const value = Number(raw);
  return Number.isFinite(value) && value > 0 ? value : fallback;
}

function hasFlag(flag) {
  return args.includes(flag);
}

function readArgValue(flag) {
  const index = args.indexOf(flag);
  if (index < 0) return undefined;
  const value = args[index + 1];
  return value && !value.startsWith("--") ? value : undefined;
}

function currentLocalTime(timeZone) {
  const parts = new Intl.DateTimeFormat("en-GB", {
    timeZone,
    hour: "2-digit",
    minute: "2-digit",
    hour12: false
  }).formatToParts(new Date());
  const hour = parts.find((part) => part.type === "hour")?.value ?? "08";
  const minute = parts.find((part) => part.type === "minute")?.value ?? "00";
  return `${hour}:${minute}`;
}

function formatInstant(value, timeZone) {
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) return value;
  return new Intl.DateTimeFormat("zh-CN", {
    timeZone,
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false
  }).format(date);
}

function truncate(value, max) {
  return value.length <= max ? value : `${value.slice(0, max - 3)}...`;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function printHelp() {
  console.log(`Usage: npm run watch:scheduler -- [options]

Options:
  --dry-run                         Use deterministic mocked calendar data.
  --once                            Run one scan/dispatch cycle and exit.
  --no-dispatch                     Only print detected trigger plans.
  --interval-ms <ms>                Watch interval. Default: 30000.
  --lookahead-hours <hours>         Calendar scan horizon. Default: 48.
  --calendar-id <id>                Lark calendar id. Default: primary.
  --user-id <id>                    User id used in produced InputEvent objects.
  --timezone <iana>                 Timezone. Default: Asia/Shanghai.
  --meeting-offset-minutes <n>      Reminder offset before meeting. Default: 2.
  --outdoor-offset-minutes <n>      Reminder offset before outdoor event. Default: 2.
  --daily-briefing-time <HH:mm>     Daily briefing local time. Default: current local time.
  --business-trip-time <HH:mm>      Trip reminder local time. Default: current local time.
  --origin-address <address>        Origin for outdoor route guidance. Default: 上海办公室.
  --lark-cli-path <path>            lark-cli wrapper path.
  --lark-identity <user|bot>        Lark identity. Default: user.
  --state-path <path>               Persist scheduler records across restarts.

Environment variables with matching WORK_ASSISTANT_* names are also supported.`);
}
