# Security boundaries

Incoming XML and file contents are untrusted data. Native plugin code is trusted application code. The receiver accepts unauthenticated positions, so a valid message does not establish that a position is true.

The application applies these input limits:

- Messages contain at most 16384 bytes.
- XML contains at most 12 nested levels and 256 elements.
- Each element contains at most 32 attributes.
- Identities, types, and callsigns contain at most 128 UTF-16 code units.
- The store contains at most 4096 event identities.
- Events expire no later than seven days after reception.

The parser rejects document type declarations, entity references, processing instructions, null bytes, and unsupported encodings. It rejects duplicate point, detail, and contact elements. Unknown elements are accepted only within bounded detail content and are then discarded.

Text fields exclude control, format, and surrogate characters. This restriction excludes supplementary Unicode characters, including many emoji. The interface presents external text as plain text, not HTML.

Coordinates must be finite decimal numbers within geographic ranges. Altitude must lie between minus 100 million and plus 100 million meters. Horizontal and vertical uncertainty must lie between zero and 100 million meters.

Timestamps must include a timezone. The stale time must follow the start time and cannot precede the event time. Events with start or event times more than five minutes ahead are rejected.

Expired events and older updates are rejected. Equal timestamps can replace an existing identity. These rules reduce accidental inconsistency but do not prevent sender impersonation or replay.

File import opens files without waiting for pipe writers and then inspects the open descriptor. It accepts regular files only and reads at most the message limit plus one byte. Export uses `QSaveFile` to replace the destination after a complete write.

The receiver starts after an explicit interface action or restoration of a saved running state. Its default binding accepts local traffic only. The bounded polling loop limits work per interface cycle, and the operating system drops traffic when its socket queue fills.

The application creates no persistent event cache and writes no message logs. Preferences include editor drafts, file directories, and the receiver address. Treat the preferences file as private data. It does not fetch remote map content, run shell commands, or load external application plugins. Qt still loads its system platform and graphics plugins as part of normal startup.

Use a trusted Qt installation and graphics driver. Native libraries and graphics drivers share the application process. This version does not provide a process sandbox, encrypted transport, or sender authentication.

# Development rules

Keep new dependencies limited to features that Qt cannot reasonably provide. Document the reason and input boundaries before adding a dependency. Do not disable certificate or hostname validation when adding encrypted transport.

Bound allocations and work before parsing external data. Add tests for malformed input and resource limits. Keep generated files and local tool paths out of Git.

Use the sanitizer preset after changes to parsing, ownership, or file handling. Run the native GUI test after interface changes. Record any untested graphics backend or target platform in the test report.

Follow [Development diagnostics](quality.md) for repeated memory, responsiveness, coverage, and source analysis runs. Exercise recoverable failure paths and successful operations after failures. Keep work and memory bounded during sessions that last for hours. Do not ignore sanitizer failures or attempt to recover from memory corruption inside the same process.

# Preferences storage

Preferences use a versioned JSON file with a one MiB limit. The file accepts regular-file reads only. Invalid files and unsupported versions remain untouched, and the interface reports the error.

Writes replace the file atomically through `QSaveFile`. The saved file permits access only to its owner. A process lock prevents concurrent instances from overwriting the same preferences.

Unknown plugin sections remain in the document. Plugins must bound every value before applying it to controls or operations. Do not store credentials in this file.

# Diagnostic output

The application disables text logging before Qt startup. Only an explicit enabled flag in the selected profile permits output. Severity, urgency, source filtering, and rate limits apply to both application and Qt messages. Logging configuration uses the bounded preferences reader and preserves unknown fields.

Messages have bounded length and cannot inject terminal control characters or extra log lines. Excess messages are dropped. Nonblocking pipe and socket writes prevent diagnostic backpressure from stopping the interface. A broken output pipe cannot terminate the application. Regular-file output still depends on filesystem latency.

Plugin code must use the shared logger and avoid private payloads. Qt diagnostics can include paths or other private context. Fatal diagnostics retain their original termination behavior even when text output is disabled. Development diagnostic tools retain their independent output.
