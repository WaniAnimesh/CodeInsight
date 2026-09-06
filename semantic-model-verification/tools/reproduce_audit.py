"""Build isolated C++ reproductions and audit their snapshots without blessing failures."""
import argparse, json, subprocess
from pathlib import Path
from audit_snapshot import audit


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--codeinsight', type=Path, required=True)
    p.add_argument('--cases', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    args=p.parse_args()
    executable=args.codeinsight.resolve()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    cases=json.loads(args.cases.read_text(encoding='utf-8-sig'))['cases']
    results=[]
    for case in cases:
        if not case['name'].replace('_','').isalnum(): raise ValueError('invalid case name')
        root=(args.output_dir/case['name']).resolve()
        root.mkdir(parents=True,exist_ok=True)
        (root/'main.cpp').write_text(case['source'],encoding='utf-8')
        commands=[dict(directory=str(root),file='main.cpp',arguments=['clang++','-std=c++20','-c','main.cpp'])]
        (root/'compile_commands.json').write_text(json.dumps(commands),encoding='utf-8')
        command=[str(executable),'build','--workspace',str(root),'--compile-commands',str(root/'compile_commands.json'),
                 '--output',str(root/'model.db'),'--jobs','1','--clean','--require-complete']
        run=subprocess.run(command,capture_output=True,text=True,timeout=120)
        (root/'build.log').write_text(run.stdout+run.stderr,encoding='utf-8')
        if run.returncode:
            result=dict(name=case['name'],build_exit=run.returncode,command=command,passed=0,failed=1,error=run.stdout+run.stderr)
        else:
            result=audit(root/'model.db',root,case)
            result.update(name=case['name'],build_exit=0,command=command)
        (root/'audit.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(('PASS' if result['failed']==0 else 'FAIL')+' '+case['name'])
        for check in result.get('checks',[]):
            if not check['passed']: print('  '+check['name']+': '+json.dumps(check['actual'])[:600])
        results.append(result)
    (args.output_dir/'results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    failures=sum(x['failed']>0 for x in results)
    print(f'Reproductions: {len(results)-failures} passed; {failures} failed')
    return int(failures>0)

if __name__=='__main__':raise SystemExit(main())
