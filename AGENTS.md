# Project instructions

Read [docs/HANDOFF.md](docs/HANDOFF.md) before starting work in a new session. Treat it as a dated snapshot. Inspect the current Git state and follow the latest user request.

Follow these standing requirements:

- Use Qt and modern C++ for this native Linux application.
- Use CMake with the Ninja generator and the repository presets.
- Keep features behind plugin interfaces where practical.
- Keep the application small, fast, and secure.
- Add external dependencies only with a documented reason.
- Use the `qt-linux-gui` skill for development when available.
- Use the `simple-english` skill for commits, code comments, and documentation when available.
- Write short, clear sentences with active verbs in commits, comments, and documentation.
- Keep project documentation and session notes under `docs/`.
- Preserve unrelated user changes.
- Test the affected behavior before committing functional changes.
- Commit all completed functional changes with Git.

Read [docs/build.md](docs/build.md) for build and test commands. Read [docs/security.md](docs/security.md) before changing input handling. Read [docs/architecture.md](docs/architecture.md) before changing plugin interfaces.

If a required skill is unavailable, report that limitation. Use the repository guidance for work that does not depend on the missing skill. Do not assume that local skill paths or temporary build tools exist on another machine.

Keep this file focused on standing instructions. Record changing state, test evidence, and known limits in [docs/HANDOFF.md](docs/HANDOFF.md). Update that handoff when those facts change.
