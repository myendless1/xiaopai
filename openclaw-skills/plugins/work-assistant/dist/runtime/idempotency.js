export class MemoryIdempotencyStore {
    records = new Map();
    get(eventId) {
        return this.records.get(eventId);
    }
    set(eventId, response) {
        this.records.set(eventId, response);
    }
}
//# sourceMappingURL=idempotency.js.map