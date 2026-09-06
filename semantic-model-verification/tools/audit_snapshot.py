"""Independent, source-backed audit of an existing CodeInsight SQLite snapshot.

This tool checks expectations supplied separately from the extractor. Failed
expectations are reported and cause exit 1; they are never blessed as goldens.
It does not claim that a finite audit proves arbitrary C++ correctness.
"""
from __future__ import annotations
from contextlib import closing
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3


def connect(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)
    db.execute('PRAGMA query_only=ON')
    return db


def audit(model: Path, workspace: Path, spec: dict) -> dict:
    results = []
    def record(name, passed, actual, expected, evidence=None):
        results.append(dict(name=name, passed=passed, actual=actual,
                            expected=expected, evidence=evidence or []))
    with closing(connect(model)) as db:
        record('SQLite integrity', db.execute('PRAGMA integrity_check').fetchall() == [('ok',)],
               db.execute('PRAGMA integrity_check').fetchall(), [['ok']])
        fk = db.execute('PRAGMA foreign_key_check').fetchall()
        record('SQLite foreign keys', not fk, fk, [])
        stored_root = db.execute("SELECT value FROM metadata WHERE key='workspace_path'").fetchone()[0]
        record('Workspace identity', Path(stored_root).resolve() == workspace.resolve(), stored_root, str(workspace.resolve()))
        mismatches = []
        for path, digest, size in db.execute('SELECT f.path,v.content_hash,v.size FROM file_versions v JOIN files f ON f.id=v.file_id'):
            file = Path(path)
            if digest.startswith('missing:'):
                if file.exists(): mismatches.append(dict(path=path, reason='previously missing input now exists'))
                continue
            if not file.is_file():
                mismatches.append(dict(path=path, reason='input disappeared'))
            else:
                content = file.read_bytes()
                if len(content) != size or hashlib.sha256(content).hexdigest() != digest:
                    mismatches.append(dict(path=path, reason='bytes differ from modeled version'))
        record('All modeled file versions match current bytes', not mismatches, mismatches, [])
        invalid_ranges = []
        # Byte coordinates, including multiline/macro expansion ranges, are
        # checked independently of the C++ validator. End offsets are exclusive.
        cache = {}
        for table in ('occurrences', 'relationships'):
            query = f'SELECT x.id,f.path,x.bl,x.bc,x.bo,x.el,x.ec,x.eo FROM {table} x JOIN file_versions v ON v.id=x.file_version_id JOIN files f ON f.id=v.file_id'
            for row in db.execute(query):
                identity, path, bl, bc, bo, el, ec, eo = row
                if path not in cache:
                    p = Path(path)
                    cache[path] = p.read_bytes() if p.is_file() else None
                data = cache[path]
                if data is None: continue
                def location(offset):
                    prefix = data[:offset]
                    return prefix.count(b'\n') + 1, offset - prefix.rfind(b'\n')
                if not (0 <= bo <= eo <= len(data)) or (bl and location(bo) != (bl, bc)) or (el and location(eo) != (el, ec)):
                    invalid_ranges.append(dict(table=table, id=identity, file=path, begin=[bl,bc,bo], end=[el,ec,eo]))
        record('Source ranges match current byte/line coordinates', not invalid_ranges,
               dict(count=len(invalid_ranges), examples=invalid_ranges[:10]), dict(count=0))
        for check in spec['checks']:
            evidence = []
            source_ok = True
            for source in check.get('sources', []):
                path = (workspace / source['path']).resolve()
                if not path.is_relative_to(workspace.resolve()):
                    raise ValueError('source evidence escapes workspace')
                content = path.read_text(encoding='utf-8-sig')
                needle = source['contains']
                offset = content.find(needle)
                source_ok &= offset >= 0
                evidence.append(dict(file=str(path), line=content[:offset].count('\n')+1 if offset >= 0 else None,
                                     contains=needle, found=offset >= 0))
            sql = check['sql'].strip()
            if not sql.lower().startswith(('select ', 'with ')):
                raise ValueError('audit checks must be read-only SELECT queries')
            actual = [list(row) for row in db.execute(sql, check.get('params', []))]
            if 'expected' in check:
                passed = actual == check['expected']
                expected = check['expected']
            else:
                expected = dict(min_rows=check['min_rows'])
                passed = len(actual) >= check['min_rows']
            record(check['name'], source_ok and passed, actual, expected, evidence)
        stats = {table: db.execute(f'SELECT count(*) FROM {table}').fetchone()[0]
                 for table in ('translation_units','symbols','logical_symbols','types','relationships','diagnostics')}
    return dict(model=str(model.resolve()), workspace=str(workspace.resolve()),
                specification=spec.get('name'), stats=stats, checks=results,
                passed=sum(r['passed'] for r in results), failed=sum(not r['passed'] for r in results))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--expectations', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    spec = json.loads(args.expectations.read_text(encoding='utf-8-sig'))
    result = audit(args.model, args.workspace, spec)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    for check in result['checks']:
        print(('PASS' if check['passed'] else 'FAIL') + ' ' + check['name'])
        if not check['passed']:
            print('  actual=' + json.dumps(check['actual'])[:1200])
            print('  expected=' + json.dumps(check['expected']))
    print(f"Audit: {result['passed']} passed; {result['failed']} failed")
    return int(result['failed'] != 0)

if __name__ == '__main__':
    raise SystemExit(main())
