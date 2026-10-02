#!/usr/bin/env python3
"""Run development diagnostics without changing the release build."""
import argparse
import gzip
import json
import os
import re
from pathlib import Path
import shutil
import shlex
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def run(command, log, timeout, env=None, cwd=ROOT):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    with log.open('w') as output:
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            output.write('\nThe diagnostic process exceeded its time limit or was interrupted.\n')
            raise RuntimeError(f'Diagnostic stopped. Read {log}') from None
    if code:
        raise RuntimeError(f'Diagnostic failed with status {code}. Read {log}')


def coverage_report(build, report, env):
    files = {}
    notes = sorted(build.rglob('*.gcno'))
    if not notes:
        raise RuntimeError('No coverage data exists.')
    for index, note in enumerate(notes):
        run(['gcov', '--json-format', '--branch-probabilities', '--branch-counts', note],
            report / f'gcov-{index}.log', 60, env, report)
        for archive in report.glob('*.gcov.json.gz'):
            with gzip.open(archive, 'rt') as stream:
                data = json.load(stream)
            for item in data['files']:
                path = Path(item['file'])
                if not path.is_absolute():
                    path = Path(data['current_working_directory']) / path
                try:
                    name = str(path.resolve().relative_to(ROOT / 'src'))
                except ValueError:
                    continue
                target = files.setdefault(name, {'lines': {}, 'functions': {}, 'branches': {}})
                for line in item['lines']:
                    key = str(line['line_number'])
                    target['lines'][key] = target['lines'].get(key, 0) + line['count']
                    for number, branch in enumerate(line.get('branches', [])):
                        branch_key = f'{key}:{number}'
                        target['branches'][branch_key] = target['branches'].get(branch_key, 0) + branch['count']
                for function in item['functions']:
                    key = function.get('demangled_name', function['name'])
                    target['functions'][key] = target['functions'].get(key, 0) + function['execution_count']
            archive.unlink()
    if not files:
        raise RuntimeError('Coverage contains no application source files.')
    (report / 'coverage.json').write_text(json.dumps(files, indent=2) + '\n')
    lines = ['# Coverage report', '', 'Unexecuted code requires review. It is not proof of dead code.', '']
    for name, data in sorted(files.items()):
        lines += [f'## {name}', '']
        for kind, counts in data.items():
            hit = sum(value > 0 for value in counts.values())
            lines.append(f'{kind.capitalize()}: {hit}/{len(counts)} executed.')
        missing = [key for key, count in data['lines'].items() if not count]
        lines += ['', 'Unexecuted lines: ' + ', '.join(missing), '']
        missing_functions = [key for key, count in data['functions'].items() if not count]
        lines += ['Unexecuted functions:', '', *[f'- `{name}`' for name in missing_functions], '']
    (report / 'coverage.md').write_text('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['quick', 'gui', 'soak', 'coverage', 'analyze'])
    parser.add_argument('--seconds', type=int, default=3600, help='Soak duration, from 1 to 86400 seconds.')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--qt-prefix', help='Optional Qt installation prefix.')
    parser.add_argument('--gui', action='store_true', help='Include native rendering in coverage or soak mode.')
    args = parser.parse_args()
    if args.gui and args.mode not in ('coverage', 'soak'):
        parser.error('--gui applies to coverage or soak mode.')
    if not 1 <= args.seconds <= 86400 or not 1 <= args.jobs <= 64:
        parser.error('Seconds must be 1..86400. Jobs must be 1..64.')
    required = ['cmake', 'ninja']
    if args.mode == 'coverage':
        required += ['gcov']
    if args.mode == 'analyze':
        required += ['cppcheck']
    if args.mode == 'gui' or args.gui:
        required += ['xvfb-run', 'xauth']
    for tool in required:
        if not shutil.which(tool):
            parser.error(f'{tool} is required for this diagnostic. Read docs/quality.md.')
    report = ROOT / 'build' / 'quality' / (time.strftime('%Y%m%d-%H%M%S') + f'-{args.mode}-{os.getpid()}')
    report.mkdir(parents=True)
    print(f'Reports: {report}', flush=True)
    env = os.environ.copy()
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1:quarantine_size_mb=16'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    env['LSAN_OPTIONS'] = 'exitcode=23'
    env['SWAK_SOAK_SECONDS'] = str(args.seconds if args.mode == 'soak' else 5)
    env.pop('SWAK_SOAK_GUI', None)
    preset = 'coverage' if args.mode == 'coverage' else 'debug' if args.mode == 'analyze' else 'sanitize'
    build = ROOT / 'build' / preset
    status = {'mode': args.mode, 'seconds': env['SWAK_SOAK_SECONDS'], 'status': 'failed'}
    try:
        run([sys.executable, '-B', ROOT / 'tests/test_quality_runner.py'], report / 'runner-tests.log', 15, env)
        run([sys.executable, '-B', ROOT / 'tests/test_version.py'], report / 'version-tests.log', 30, env)
        command = ['cmake', '--preset', preset]
        if args.qt_prefix:
            command += [f'-DCMAKE_PREFIX_PATH={args.qt_prefix}']
        run(command, report / 'configure.log', 120, env)
        if args.mode == 'analyze':
            database = json.loads((build / 'compile_commands.json').read_text())
            database = [item for item in database if Path(item['file']).is_relative_to(ROOT / 'src')]
            stubs = report / 'moc'
            stubs.mkdir()
            for item in database:
                for name in re.findall(r'#include "([^"/]+\.moc)"', Path(item['file']).read_text()):
                    (stubs / name).write_text('// Generated Qt code is tested by the compiler.\n')
                arguments = item.get('arguments') or shlex.split(item['command'])
                item['arguments'] = ['-I' + str(stubs) if arg.startswith('-I') and arg.endswith('_autogen/include')
                                     else arg for arg in arguments]
                item.pop('command', None)
            project = report / 'compile_commands.json'
            project.write_text(json.dumps(database))
            run(['cppcheck', f'--project={project}', '--enable=warning,style,performance,portability,unusedFunction',
                 '--error-exitcode=1', '--inline-suppr', '--suppress=missingIncludeSystem', '--std=c++20',
                 '--library=qt', '--library=posix', '--check-level=exhaustive'],
                report / 'analysis.log', 900, env)
        else:
            run(['cmake', '--build', '--preset', preset, '--parallel', str(args.jobs)], report / 'build.log', 900, env)
            if args.mode == 'coverage':
                for data in build.rglob('*.gcda'):
                    data.unlink()
            if args.mode in ('quick', 'coverage'):
                run(['ctest', '--test-dir', build, '--output-on-failure', '--timeout', '60'], report / 'tests.log', 240, env)
            if args.mode == 'soak':
                command = [build / 'swak_soak_tests']
                if args.gui:
                    env['QT_QPA_PLATFORM'] = 'xcb'
                    env['SWAK_SOAK_GUI'] = '1'
                    command = ['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24 -nolisten tcp'] + command
                run(command, report / 'soak.log', args.seconds + 30, env, report)
            if args.mode == 'gui' or (args.mode == 'coverage' and args.gui):
                env['QT_QPA_PLATFORM'] = 'xcb'
                run(['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24 -nolisten tcp', build / 'swak_gui_tests'],
                    report / 'gui.log', 180, env, report)
            if args.mode == 'coverage':
                coverage_report(build, report, env)
        status['status'] = 'passed'
    except (RuntimeError, OSError) as error:
        status['error'] = str(error)
        print(error, file=sys.stderr)
    finally:
        (report / 'result.json').write_text(json.dumps(status, indent=2) + '\n')
    return 0 if status['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
