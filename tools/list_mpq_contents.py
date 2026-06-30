"""list_mpq_contents.py — READ-ONLY. List the (listfile) of each custom patch MPQ
so we can see which DBCs/files each contains (esp. patch-4/5/6 root + Druid form file).
"""
import os, glob
import mpyq

ROOT   = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data'
LOCALE = r'C:\Users\Donald Lee Irwin Esq\Desktop\3.3.5\Data\enUS'

TARGETS = [
    os.path.join(ROOT, 'patch-4.MPQ'),
    os.path.join(ROOT, 'patch-5.MPQ'),
    os.path.join(ROOT, 'patch-6.MPQ'),
    os.path.join(LOCALE, 'patch-enUS-3.MPQ'),
    os.path.join(LOCALE, 'patch-enUS-4.MPQ'),
]

for m in TARGETS:
    print(f"\n===== {os.path.relpath(m, ROOT)} ({os.path.getsize(m):,} bytes) =====")
    try:
        arch = mpyq.MPQArchive(m)
        lf = arch.read_file('(listfile)')
        if lf:
            names = lf.decode('ascii', 'replace').replace('\r\n','\n').replace('\r','\n').split('\n')
            names = [n for n in names if n.strip()]
            for n in sorted(names):
                print("   ", n)
            print(f"   [{len(names)} files in listfile]")
        else:
            print("   (no listfile present)")
    except Exception as e:
        print(f"   mpyq failed: {e}")
