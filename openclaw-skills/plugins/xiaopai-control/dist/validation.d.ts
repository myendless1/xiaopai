import type { ValidationResult, XiaopaiCommand, XiaopaiValidationError } from "./contracts.js";
export declare function extractCommandInput(value: unknown): unknown;
export declare function validateXiaopaiCommand(value: unknown): ValidationResult<XiaopaiCommand>;
export declare function rejectedResultForValidation(error: XiaopaiValidationError): import("./contracts.js").XiaopaiCommandResult;
