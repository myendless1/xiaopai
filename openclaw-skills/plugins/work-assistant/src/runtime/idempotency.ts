import type { StructuredResponse } from "../contracts.js";

export interface IdempotencyStore {
  get(eventId: string): Promise<StructuredResponse | undefined> | StructuredResponse | undefined;
  set(eventId: string, response: StructuredResponse): Promise<void> | void;
}

export class MemoryIdempotencyStore implements IdempotencyStore {
  private readonly records = new Map<string, StructuredResponse>();

  get(eventId: string): StructuredResponse | undefined {
    return this.records.get(eventId);
  }

  set(eventId: string, response: StructuredResponse): void {
    this.records.set(eventId, response);
  }
}
