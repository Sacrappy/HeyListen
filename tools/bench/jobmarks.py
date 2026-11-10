r"""(TICK-042, diagnostic) A job-marked lift for tools/bench/jobgraph.py: wraps the game's job calls in hos_jt_job() marks
(host/hos_jobtrace.c), which a HOS_JOBTRACE run records as JT_JOB_B / JT_JOB_E. Patches the lifted C in place and copies
each file it changes into <backup dir> first; copy them back (and build again) to return to the shipped lift. The marks
change lifted code: never measure fps with such a build (and its PGO profile no longer matches those functions).

    python -I tools/bench/jobmarks.py %LOCALAPPDATA%/HookshotBuilder/sa/gen <backup dir>

The default set (Castle Town Market, TICK-042): every indirect call in the BaseProc dispatcher f_8004ab40, the job-group
runner f_8007fe60 (main thread and module workers), the thread loop f_80df25c0, the dispatcher's sub-runners
f_80049280, f_8004fee0, f_8004fa40 (their tail jumps too) and the dispatcher's direct calls of them.
"""
import re, sys, glob, os, shutil
gen, bkdir = sys.argv[1], sys.argv[2]
funcs = ["f_8004ab40", "f_8007fe60", "f_80df25c0", "f_80049280", "f_8004fee0", "f_8004fa40"]
DIRECT = ["0x8004ba54", "0x8004b694", "0x8004ac6c"]   # (f_8004ab40 -> f_80049280, f_8004fee0, f_8004fa40)
os.makedirs(bkdir, exist_ok=True)
DECL = "void hos_jt_job(uint64_t obj, uint64_t target, uint64_t site, int begin);\n"
ICALL = re.compile(r"\{ uint64_t _t = (c->x\[\d+\]); c->x\[30\] = (0x[0-9a-f]+)ULL; A64_ICALL\(c, _t\); \}")
IJUMP = re.compile(r"A64_IJUMP\(c, (c->x\[\d+\])\); return;")
for f in funcs:
    head = "\nvoid " + f + "(Ctx *__restrict A64_NOESC c) {\n"
    for p in glob.glob(os.path.join(gen, "code_*.c")):
        s = open(p, encoding="utf-8").read()
        if head not in s:
            continue
        bk = os.path.join(bkdir, os.path.basename(p))
        if not os.path.exists(bk):
            shutil.copy2(p, bk)
        i = s.index(head); j = s.index("\n}\n", i)
        body = s[i:j]
        n = len(ICALL.findall(body))
        body = ICALL.sub(lambda m: "{ uint64_t _t = %s; c->x[30] = %sULL; const uint64_t _jo = c->x[0]; hos_jt_job(_jo, _t, %sULL, 1); "
                         "A64_ICALL(c, _t); hos_jt_job(_jo, _t, %sULL, 0); }" % (m.group(1), m.group(2), m.group(2), m.group(2)), body)
        for site in DIRECT:
            dc = re.compile(r"c->x\[30\] = %sULL; (f_([0-9a-f]+)\(c\);)" % site)
            n += len(dc.findall(body))
            body = dc.sub(lambda m: "{ const uint64_t _jo = c->x[0]; c->x[30] = %sULL; hos_jt_job(_jo, 0x%sULL, %sULL, 1); %s "
                          "hos_jt_job(_jo, 0x%sULL, %sULL, 0); }" % (site, m.group(2), site, m.group(1), m.group(2), site), body)
        n += len(IJUMP.findall(body))
        body = IJUMP.sub(lambda m: "{ const uint64_t _jt = %s, _jo = c->x[0]; hos_jt_job(_jo, _jt, 0xffffffffULL, 1); A64_IJUMP(c, _jt); "
                         "hos_jt_job(_jo, _jt, 0xffffffffULL, 0); } return;" % m.group(1), body)
        s = s[:i] + body + s[j:]
        if DECL not in s:
            s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)
        open(p, "w", encoding="utf-8", newline="\n").write(s)
        print(f, p, n, "sites")
