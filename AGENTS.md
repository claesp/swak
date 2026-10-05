# Project instructions

Read [docs/HANDOFF.md](docs/HANDOFF.md) before starting work in a new session. Treat it as a dated snapshot. Inspect the current Git state and follow the latest user request.

Follow these standing requirements:

- Use Qt and modern C++ for this native Linux application.
- Follow docs/coding-style.md and run tools/style.py before committing code changes.
- Use CMake with the Ninja generator and the repository presets.
- Keep features behind plugin interfaces where practical.
- Use the shared core API for every plugin and embed a MANIFEST with its metadata.
- Update `docs/plugin-api.md` when plugin contracts or roles change.
- Keep the application small, fast, and secure.
- Cache animation configuration in runtime fields. Refresh it only when configuration changes, never during frames, animation ticks, or incoming position updates.
- Harden all code for long sessions and recoverable failures.
- Exercise all reachable code paths, including errors, cancellation, and shutdown, with tests.
- Review coverage gaps and record paths that remain untested.
- Regularly inspect the code for dead code, unsafe ownership, leaks, unbounded loops, and other common faults.
- Run the applicable diagnostics in `docs/quality.md` during development and all modes before a release.
- Keep recoverable failures from crashing the application or blocking the interface.
- Keep application text logging disabled until the user enables it in configuration.
- Route application and plugin diagnostics through the shared severity and urgency logger.
- Expose plugin configuration through the shared Settings interface.
- Do not hide memory corruption with catch-all handlers or fatal signal recovery.
- Add external dependencies only with a documented reason.
- Use the `cpp-skill-focused-on-safety` skill to write and review C++ and Qt code.
- Use the `simplified-technical-english-asd-ste100` skill for commits, code comments, and documentation.
- Write short, clear sentences with active verbs in commits, comments, and documentation.
- Keep project documentation and session notes under `docs/`.
- Preserve unrelated user changes.
- Test the affected behavior before committing functional changes.
- Rebuild the release executable after every commit.
- Always rebuild the release executable with the CMake release preset before completing a task that changes the application.
- Report any release build failure before handing the task back to the user.

# Git

- Commit all completed functional changes with Git.
- Keep commit messages very short and very concise. Write one summary line of 50 characters or fewer in the imperative mood.
- Add a commit body only when the summary cannot give the reason. Keep the body to a few short lines.
- Do not add attribution trailers, such as `Co-Authored-By`, to commit messages. [docs/README.md](docs/README.md) records tool attribution.
- Do not commit build output, logs, or other files under `build/`.
- Do not rewrite published history or force-push unless the user asks for it.

# References

Read [docs/build.md](docs/build.md) for build and test commands. Read [docs/security.md](docs/security.md) before changing input handling. Read [docs/architecture.md](docs/architecture.md) before changing plugin interfaces.

If a required skill is unavailable, report that limitation. Use the repository guidance for work that does not depend on the missing skill. Do not assume that local skill paths or temporary build tools exist on another machine.

Keep this file focused on standing instructions. Record changing state, test evidence, and known limits in [docs/HANDOFF.md](docs/HANDOFF.md). Update that handoff when those facts change.
