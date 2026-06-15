import type { StructuredResponse } from "../contracts.js";
export interface IdempotencyStore {
    get(eventId: string): Promise<StructuredResponse | undefined> | StructuredResponse | undefined;
    set(eventId: string, response: StructuredResponse): Promise<void> | void;
}
export declare class MemoryIdempotencyStore implements IdempotencyStore {
    private readonly records;
    get(eventId: string): StructuredResponse | undefined;
    set(eventId: string, response: StructuredResponse): void;
}
