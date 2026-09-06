"""Exercise CLI failure gates and ensure failures preserve the published snapshot."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--codeinsight', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    executable = args.codeinsight.resolve()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = root / 'main.cpp'
    database = root / 'compile_commands.json'
    model = root / 'model.db'
    source.write_text('int main() { return 0; }\n', encoding='utf-8')
    database.write_text(json.dumps([dict(directory=str(root), file='main.cpp',
        arguments=['clang++', '-std=c++20', '-c', 'main.cpp'])]), encoding='utf-8')

    def run(*arguments):
        return subprocess.run([str(executable), *map(str, arguments)], capture_output=True, text=True, timeout=45)

    build = ['build', '--workspace', root, '--compile-commands', database, '--output', model, '--require-complete']
    for value in ['-1', '+2', '2workers', '', '18446744073709551616']:
        result = run(*build, '--jobs', value)
        assert result.returncode != 0 and '--jobs must be' in result.stderr, (value, result)
    result = run(*build, '--jobs', '18446744073709551615', '--clean')
    assert result.returncode == 0, result.stderr
    original = hashlib.sha256(model.read_bytes()).hexdigest()
    result = run('export', '--model', model, '--output', model)
    assert result.returncode != 0 and 'must not overwrite' in result.stderr, result
    assert hashlib.sha256(model.read_bytes()).hexdigest() == original
    source.write_text('int main( {\n', encoding='utf-8')
    result = run(*build, '--jobs', '1')
    assert result.returncode != 0 and 'complete-model gate failed' in result.stderr, result
    assert hashlib.sha256(model.read_bytes()).hexdigest() == original, 'failed build replaced published model'
    source.write_text('int main() { return 0; }\n', encoding='utf-8')
    result = run(*build, '--jobs', '0')
    assert result.returncode == 0, result.stderr
    # Older schema versions must be re-extracted, never silently reused.
    with closing(sqlite3.connect(model)) as connection:
        connection.execute("UPDATE metadata SET value='3' WHERE key='schema_version'")
        connection.commit()
    result = run(*build, '--jobs', '1')
    assert result.returncode == 0 and 'Extracted TUs: 1; reused: 0' in result.stdout, result
    result = run('validate-model', '--model', model)
    assert result.returncode == 0, result.stderr
    # Persisted symbol claims must match their live occurrence evidence.
    with closing(sqlite3.connect(model)) as connection:
        connection.execute('UPDATE symbols SET function_flags=function_flags|8192 WHERE name=\'main\'')
        connection.commit()
    result = run('validate-model', '--model', model)
    assert result.returncode != 0 and 'lifetime.symbol-properties' in result.stderr, result
    print('CLI input, publication, recovery, version, and evidence gates passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
