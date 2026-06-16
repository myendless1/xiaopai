const SPEECH_ENDING_PUNCTUATION = /[。！？!?；;]$/;

export function normalizeSpeechTextForVoice(text: string): string {
  return stripMarkdownSyntax(normalizeMarkdownTables(String(text ?? "")))
    .replace(/\r\n?/g, "\n")
    .replace(/\u00a0/g, " ")
    .replace(/[✅✔☑❌✖]/g, "")
    .replace(/\p{Extended_Pictographic}/gu, "")
    .replace(/[ \t]*\n[ \t]*/g, " ")
    .replace(/[ \t]+/g, " ")
    .replace(/\s*([，。！？、；：])\s*/g, "$1")
    .replace(/\s+([,.!?;:])/g, "$1")
    .trim();
}

function normalizeMarkdownTables(text: string): string {
  const prepared = text.replace(/\|\s+(?=\|)/g, "|\n").replace(/\r\n?/g, "\n");
  const lines = prepared.split("\n");
  const output: string[] = [];

  for (let index = 0; index < lines.length; index += 1) {
    const originalLine = lines[index] ?? "";
    const { prefix, tableLine } = splitTablePrefix(originalLine, lines[index + 1]);
    const nextLine = (lines[index + 1] ?? "").trim();

    if (isMarkdownTableRow(tableLine) && isMarkdownTableSeparatorRow(nextLine)) {
      if (prefix) output.push(prefix);
      const headers = splitMarkdownTableRow(tableLine);
      const rows: string[] = [];
      index += 2;

      while (index < lines.length) {
        const rowLine = (lines[index] ?? "").trim();
        if (!isMarkdownTableRow(rowLine) || isMarkdownTableSeparatorRow(rowLine)) break;
        const row = formatMarkdownTableRow(headers, splitMarkdownTableRow(rowLine));
        if (row) rows.push(row);
        index += 1;
      }

      index -= 1;
      if (rows.length > 0) output.push(withSentenceEnding(rows.join("；")));
      continue;
    }

    output.push(originalLine);
  }

  return output.join("\n");
}

function splitTablePrefix(line: string, nextLine: string | undefined): { prefix: string; tableLine: string } {
  const trimmed = line.trim();
  const pipeIndex = trimmed.indexOf("|");
  if (pipeIndex <= 0) return { prefix: "", tableLine: trimmed };

  const candidate = trimmed.slice(pipeIndex).trim();
  if (!isMarkdownTableRow(candidate) || !isMarkdownTableSeparatorRow(String(nextLine ?? "").trim())) {
    return { prefix: "", tableLine: trimmed };
  }

  return {
    prefix: trimmed.slice(0, pipeIndex).trim(),
    tableLine: candidate
  };
}

function isMarkdownTableRow(line: string): boolean {
  const trimmed = line.trim();
  return trimmed.startsWith("|") && trimmed.endsWith("|") && splitMarkdownTableRow(trimmed).length >= 2;
}

function isMarkdownTableSeparatorRow(line: string): boolean {
  const cells = splitMarkdownTableRow(line);
  return cells.length >= 2 && cells.every((cell) => /^:?-{3,}:?$/.test(cell.replace(/\s+/g, "")));
}

function splitMarkdownTableRow(line: string): string[] {
  return line
    .trim()
    .replace(/^\|/, "")
    .replace(/\|$/, "")
    .split("|")
    .map(cleanMarkdownCell);
}

function formatMarkdownTableRow(headers: string[], cells: string[]): string {
  const parts: string[] = [];
  const count = Math.max(headers.length, cells.length);
  for (let index = 0; index < count; index += 1) {
    const cell = (cells[index] ?? "").trim();
    if (!cell) continue;
    const header = (headers[index] ?? "").trim();
    if (!header || isHeaderSafeToOmit(header)) {
      parts.push(cell);
    } else {
      parts.push(`${header}${cell}`);
    }
  }
  return parts.join("，");
}

function isHeaderSafeToOmit(header: string): boolean {
  return /^(时间|日期|时段|开始|结束|内容|事项|标题|名称|事件|日程)$/i.test(header.replace(/\s+/g, ""));
}

function cleanMarkdownCell(value: string): string {
  return value
    .trim()
    .replace(/(\*\*|__)(.*?)\1/g, "$2")
    .replace(/~~(.*?)~~/g, "$1")
    .replace(/`([^`]+)`/g, "$1")
    .replace(/[*_`]+/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

function withSentenceEnding(value: string): string {
  const trimmed = value.trim();
  if (trimmed === "" || SPEECH_ENDING_PUNCTUATION.test(trimmed)) return trimmed;
  return `${trimmed}。`;
}

function stripMarkdownSyntax(text: string): string {
  const withoutBlocks = text
    .replace(/```[A-Za-z0-9_-]*\n?/g, "")
    .replace(/```/g, "")
    .replace(/!\[([^\]]*)\]\([^)]+\)/g, "$1")
    .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
    .replace(/`([^`]+)`/g, "$1");

  return withoutBlocks
    .split("\n")
    .map(stripMarkdownLinePrefix)
    .join("\n")
    .replace(/(\*\*|__)(.*?)\1/g, "$2")
    .replace(/~~(.*?)~~/g, "$1")
    .replace(/(^|[^\w])\*([^*\n]+)\*/g, "$1$2")
    .replace(/(^|[^\w])_([^_\n]+)_/g, "$1$2")
    .replace(/^[|:\-\s]+$/gm, "")
    .replace(/\s*\|\s*/g, "，")
    .replace(/[*_`]+/g, "");
}

function stripMarkdownLinePrefix(line: string): string {
  return line
    .trim()
    .replace(/^#{1,6}\s+/, "")
    .replace(/^>\s?/, "")
    .replace(/^[-*+]\s+/, "")
    .replace(/^\d+[.)]\s+/, "");
}
