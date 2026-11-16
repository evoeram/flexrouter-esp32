# -*- coding: utf-8 -*-
"""
Извлечение переменных из NN_pnk_154m_v1_40.gau (PT Tu-154M, ПНК)
и генерация профиля FlexRouter по аналогии с profile001.ap.

Как это работает
================
1. NN_pnk_154m_v1_40.gau — PE32 DLL. Таблица релокаций .reloc содержит все
   абсолютные адреса, на которые ссылается код: множество REFS.
2. PNK_154M.map — map-файл близкой сборки: полный словарь глобальных
   переменных (имя, MSVC-тип, RVA, obj-файл подсистемы). Офсеты map
   НЕ совпадают с поставляемым GAU (другая сборка) — нужен пересчёт.
3. Якоря: profile001.ap даёт 245 точных пар (map_rva -> shipped_rva)
   для символов PNK. Дополнительно добавляются точные якоря ?gauge_name_<X>
   (указатель на строку "PNK_<X>" в .rdata поставляемого GAU).
4. Дельта (map -> shipped) интерполируется кусочно-линейно по якорям.
5. Предсказанные адреса проверяются по множеству REFS (релокации) —
   совпадение = сильное подтверждение правильности офсета.
6. Размер переменной берётся из MSVC-манглинга типа (@@3HA = int и т.д.).

Выход:
  analysis/variables_pnk_154m.csv
  analysis/named_variables.txt
  analysis/GAU_structure.md
  profile002.ap
"""
import struct, re, csv, uuid, bisect, sys
from pathlib import Path
from collections import Counter, OrderedDict
import xml.etree.ElementTree as ET

WS     = Path(__file__).resolve().parent.parent
GAU    = WS / r"PT Tu-154M v2.04 FULL\NN_pnk_154m_v1_40.gau"
MAPDIR = WS / r"PT Tu-154M v2.04 FULL\204_beta-2\Aircraft\PT_Tu-154M\Info\gau_maps"
PROF1  = WS / r"PT Tu-154M v2.04 FULL\profile001.ap"
OUTDIR = WS / "analysis"
MODULE = "NN_pnk_154m_v1_40.GAU"
BASE   = 0x10000000
OUTDIR.mkdir(exist_ok=True)

data = GAU.read_bytes()

# ------------------------------------------------------------------ PE ---
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
nsec     = struct.unpack_from("<H", data, e_lfanew + 6)[0]
sizeopt  = struct.unpack_from("<H", data, e_lfanew + 20)[0]
sec0     = e_lfanew + 24 + sizeopt
sections = []
for i in range(nsec):
    o = sec0 + 40 * i
    name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
    vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
    sections.append((name, vaddr, vsize, rptr, rsize))
SEC = {n: (va, vs, rp, rs) for n, va, vs, rp, rs in sections}

def rva2off(rva):
    for name, vaddr, vsize, rptr, rsize in sections:
        if vaddr <= rva < vaddr + vsize:
            d = rva - vaddr
            return rptr + d if d < rsize else None
    return None

d_va, d_vs, d_rp, d_rs = SEC[".data"]      # .data (включая bss в хвосте)
r_va, r_vs, r_rp, r_rs = SEC[".rdata"]

# ------------------------------------------------- relocation target set ---
_, _, rp, rz = SEC[".reloc"]
refs = set()
off = rp
while off < rp + rz:
    page, bsz = struct.unpack_from("<II", data, off)
    if bsz == 0:
        break
    for j in range((bsz - 8) // 2):
        e = struct.unpack_from("<H", data, off + 8 + j * 2)[0]
        if e >> 12 == 3:
            fo = rva2off(page + (e & 0xFFF))
            if fo is not None:
                refs.add(struct.unpack_from("<I", data, fo)[0])
    off += bsz
print(f"[i] relocation targets: {len(refs)}")

# ----------------------------------------------------------- map parse ---
def parse_map(path):
    raw = path.read_text(errors="replace").splitlines()
    lines = []
    for ln in raw:
        if re.match(r"^ [0-9a-f]{4}:", ln):
            lines.append(ln)
        elif lines and ln.strip():
            lines[-1] += ln
    syms = []
    for ln in lines:
        m = re.match(r"^ ([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+"
                     r"([0-9a-f]{8})(?:\s+f)?\s+(\S+\.obj)", ln)
        if m:
            syms.append(dict(seg=int(m.group(1), 16), off=int(m.group(2), 16),
                             name=m.group(3), rva=int(m.group(4), 16) - BASE,
                             obj=m.group(5)))
    return syms

map_syms = parse_map(MAPDIR / "PNK_154M.map")
print(f"[i] PNK_154M.map symbols: {len(map_syms)}")

mvars = OrderedDict()      # short name -> first occurrence
gauge_names = {}           # short -> map rva (символы ?gauge_name_<X>@@3PADA)
for s in map_syms:
    if s["seg"] != 3:
        continue
    m = re.match(r"\?([A-Za-z_]\w*)@@3(.+)$", s["name"])
    if not m:
        continue
    short, tc = m.group(1), m.group(2)
    mvars.setdefault(short, dict(rva=s["rva"], tc=tc, name=s["name"], obj=s["obj"]))
    if tc == "PADA" and short.startswith("gauge_name_"):
        gauge_names[short[len("gauge_name_"):]] = s["rva"]
print(f"[i] seg3 variables: {len(mvars)}, gauge_name symbols: {len(gauge_names)}")

# ----------------------------------------------------- type classifier ---
# MSVC data types: ?name@@3<type>A, где type:
#   H int, I unsigned int, J long, K unsigned long, M float, N double,
#   O long double, D char, E unsigned char, F short, G unsigned short,
#   _N bool, _J __int64, _K unsigned __int64; PA* — указатель;
#   PAY0<dim><type> — указатель на массив.
TYPE_MAP = {
    "H": ("FourBytes",      "int"),            "I": ("FourBytes",      "unsigned int"),
    "J": ("FourBytes",      "long"),           "K": ("FourBytes",      "unsigned long"),
    "M": ("FourBytesFloat", "float"),          "N": ("EightByteFloat", "double"),
    "O": ("EightByteFloat", "long double"),    "C": ("ByteSigned",     "signed char"),
    "D": ("ByteSigned",     "char"),           "E": ("Byte",           "unsigned char"),
    "F": ("TwoBytesSigned", "short"),          "G": ("TwoBytes",       "unsigned short"),
    "_N": ("Byte",          "bool"),           "_J": ("EightBytesSigned", "__int64"),
    "_K": ("EightBytes",    "unsigned __int64"),
}

def classify(tc):
    """-> (flex_size|None, c_type, kind, note)"""
    if tc.startswith("PAY0"):
        # массив: PAY0<код размерности><тип>A
        m = re.match(r"PAY0[A-Z]*?(H|I|J|K|M|N|O|C|D|E|F|G|_N|_J|_K)", tc)
        base = m.group(1) if m else None
        if base in TYPE_MAP:
            s, t = TYPE_MAP[base]
            return s, f"{t}[]", "array", "первый элемент массива"
        return None, tc, "array", ""
    if tc.startswith("P"):
        return None, tc, "pointer", ""          # указатели не годятся для железа
    if tc.startswith(("U", "V")):
        return None, tc, "struct", ""
    base = tc[:-1] if tc.endswith("A") and tc not in TYPE_MAP else tc
    if base in TYPE_MAP:
        s, t = TYPE_MAP[base]
        return s, t, "scalar", ""
    return None, tc, "other", ""

# ----------------------------------------- anchors from profile001 ---
root = ET.fromstring(PROF1.read_text(encoding="utf-16"))
anchors = {}
prof1_pnk_count = 0
for v in root.iter("Variable"):
    if v.get("Type") != "MemoryPatchVariable":
        continue
    if (v.get("ModuleName") or "").upper() != MODULE.upper():
        continue
    prof1_pnk_count += 1
    m = re.match(r"\?([A-Za-z_]\w*)(\+0x[0-9A-Fa-f]+)?$", v.get("NameInMapFile") or "")
    if m:
        anchors[m.group(1)] = BASE + int(v.get("Offset"), 16)
print(f"[i] profile001: {prof1_pnk_count} PNK vars, {len(anchors)} symbol anchors")

# ------------------------------------- anchors: gauge_name_* (не сработало,
# эти переменные инициализируются динамически — оставлен только опыт) -------

# ------------------------------------------------- delta interpolation ---
def build_pts(anchor_pairs):
    return sorted((mrva - BASE, ship - BASE) for mrva, ship in anchor_pairs)

def interp_delta(pts, x):
    if not pts:
        return 0
    if x <= pts[0][0]:
        return pts[0][1] - pts[0][0]
    if x >= pts[-1][0]:
        return pts[-1][1] - pts[-1][0]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x0 <= x <= x1:
            if x1 == x0:
                return y0 - x0
            t = (x - x0) / (x1 - x0)
            return int(round((y0 - x0) + t * ((y1 - x1) - (y0 - x0))))
    return 0

pairs = [(BASE + mvars[a]["rva"], ship) for a, ship in anchors.items() if a in mvars]
pts = build_pts(pairs)
print(f"[i] anchors: {len(pairs)}")
print("[i] anchor map_rva range:",
      f"0x{min(p[0] for p in pairs)-BASE:X}..0x{max(p[0] for p in pairs)-BASE:X}")

# честная leave-one-out проверка точности интерполяции
errs = []
for mrva, ship in pairs:
    sub = [(a, b) for a, b in pairs if a != mrva]
    pred = BASE + (mrva - BASE) + interp_delta(sub, mrva - BASE)
    errs.append(abs(pred - ship))
errs.sort()
n = len(errs)
print(f"[i] leave-one-out error: median={errs[n//2]} p90={errs[int(n*0.9)]} "
      f"max={errs[-1]}, exact={sum(1 for e in errs if e == 0)}/{n}")

# ----------------------------------------------------- gap-based sizes ---
seg3_sorted = sorted(s["rva"] for s in map_syms if s["seg"] == 3)
def gap_size(rva):
    i = bisect.bisect_right(seg3_sorted, rva)
    return seg3_sorted[i] - rva if i < len(seg3_sorted) else None

# ------------------------------------------- build variable records ---
rows = []
for short, v in mvars.items():
    fr_size, ctype, kind, note = classify(v["tc"])
    if kind in ("pointer", "struct", "other"):
        continue
    if short in anchors:
        ship = anchors[short] - BASE
        source, verified = "profile001 (точный)", ship + BASE in refs
    else:
        d = interp_delta(pts, v["rva"])
        ship = v["rva"] + d
        verified = ship + BASE in refs
        source = "map -> интерполяция"
    rows.append(dict(name=short, mangled=v["name"], ctype=ctype, kind=kind,
                     fr_size=fr_size, note=note, map_rva=v["rva"],
                     ship_rva=ship, obj=v["obj"], source=source,
                     verified=verified, gap=gap_size(v["rva"])))

cnt = Counter((r["kind"], r["verified"]) for r in rows)
print("[i] variable kinds:", dict(cnt))
scalars = [r for r in rows if r["kind"] == "scalar"]
print(f"[i] scalars: {len(scalars)}, verified: {sum(r['verified'] for r in scalars)}")

# ------------------------------------------ named variables (FS9) ---
named = []
i, end = r_rp, r_rp + r_rs
while i < end:
    j = i
    while j < end and 0x20 <= data[j] <= 0x7E:
        j += 1
    if j - i >= 4:
        s = data[i:j].decode("ascii")
        if re.fullmatch(r"[A-Z][A-Z0-9_]{3,}", s) and not re.search(r"_{3,}|^STL|^STD", s):
            named.append((r_va + (i - r_rp), s))
    i = j + 1
named.sort()
print(f"[i] named-variable strings in GAU: {len(named)}")
with open(OUTDIR / "named_variables.txt", "w", encoding="utf-8") as f:
    f.write("Именованные переменные FS9 (строки .rdata поставляемого NN_pnk_154m_v1_40.gau)\n")
    f.write("Регистрируются через register_named_variable/check_named_variable.\n\n")
    f.write("RVA        Имя\n")
    f.write("-" * 60 + "\n")
    for rva, s in named:
        f.write(f"0x{rva:06X}  {s}\n")

# ------------------------------------------------------------- CSV ---
with open(OUTDIR / "variables_pnk_154m.csv", "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f, delimiter=";")
    w.writerow(["Имя", "C-тип", "Размер FlexRouter", "RVA (map)", "RVA (GAU, hex)",
                "Offset для профиля", "ModuleName", "Подсистема (obj)",
                "Источник офсета", "Подтверждено релокациями",
                "Примечание", "Манглированный символ"])
    for r in sorted(rows, key=lambda x: (not x["verified"], x["obj"], x["ship_rva"])):
        w.writerow([r["name"], r["ctype"], r["fr_size"], hex(r["map_rva"]),
                    f"0x{r['ship_rva']:X}", f"{r['ship_rva']:X}", MODULE,
                    r["obj"], r["source"],
                    "да" if r["verified"] else "НЕТ — проверить", r["note"],
                    r["mangled"]])
print(f"[+] CSV: {OUTDIR / 'variables_pnk_154m.csv'} ({len(rows)} строк)")

# --------------------------------------------------- profile002.ap ---
PANELS = [
    ("pnk-azs",  "PNK Оверхед/АЗС (azs_*)"),
    ("pnk-tks",  "PNK ТКС/ПНП (tks_*, pnp_*)"),
    ("pnk-absu", "PNK АБСУ (absu_*, at_*, stab_*)"),
    ("pnk-rsbn", "PNK РСБН (rsbn_*)"),
    ("pnk-nvu",  "PNK НВУ (nvu_*)"),
    ("pnk-diss", "PNK ДИСС (diss_*)"),
    ("pnk-elec", "PNK Электрика (elektro_*)"),
    ("pnk-svs",  "PNK СВС (svs_*)"),
    ("pnk-radio","PNK Радио (ark_*, uvid_*, radio_*)"),
    ("pnk-fice", "PNK СПУ/свет (fice_*)"),
    ("pnk-eng",  "PNK Двигатели (eng_*, rev_*, thrust_*)"),
    ("pnk-misc", "PNK Прочее"),
]
PREF = {
    "pnk-azs":  ("azs_", "lamp_", "blinker_"),
    "pnk-tks":  ("tks_", "pnp_"),
    "pnk-absu": ("absu_", "at_", "stab_", "trim_"),
    "pnk-rsbn": ("rsbn_",),
    "pnk-nvu":  ("nvu_",),
    "pnk-diss": ("diss_",),
    "pnk-elec": ("elektro_",),
    "pnk-svs":  ("svs_",),
    "pnk-radio": ("ark_", "uvid_", "radio_"),
    "pnk-fice": ("fice_",),
    "pnk-eng":  ("eng_", "rev_", "thrust_"),
}

def panel_of(name):
    low = name.lower()
    for pid, prefs in PREF.items():
        if low.startswith(prefs):
            return pid
    return "pnk-misc"

guids = {pid: str(uuid.uuid4()) for pid, _ in PANELS}
sel = [r for r in rows if r["fr_size"]]
sel.sort(key=lambda r: (panel_of(r["name"]), r["ship_rva"]))

out = ['<?xml version="1.0" encoding="utf-16"?>',
       f'<FlexRouterProfile Type="Public" '
       f'Name="ПТ Ту-154 PNK v1.40 — извлечённые переменные GAU (авто)" '
       f'Id="{uuid.uuid4()}">',
       "  <Panels>"]
for pid, pname in PANELS:
    if any(panel_of(r["name"]) == pid for r in sel):
        out.append(f'    <Panel Id="{guids[pid]}" Name="{pname}" PowerFormula="" />')
out.append("  </Panels>")
out.append('  <Variables ProcessToManage="fs9">')
for r in sel:
    pid = guids[panel_of(r["name"])]
    status = ("офсет подтверждён таблицей релокаций GAU" if r["verified"]
              else "офсет расчётный (map+интерполяция) — требует проверки")
    desc = f"C-тип {r['ctype']}; {r['obj']}; {r['source']}; {status}"
    if r["note"]:
        desc = r["note"] + "; " + desc
    out.append(f'    <Variable Type="MemoryPatchVariable" Id="{uuid.uuid4()}" '
               f'PanelId="{pid}" Name="{r["name"]}" Description="{desc}" '
               f'Offset="{r["ship_rva"]:X}" Size="{r["fr_size"]}" '
               f'ModuleName="{MODULE}" NameInMapFile="?{r["name"]}+0x0" />')
out.append("  </Variables>")
out.append("</FlexRouterProfile>")

prof = WS / "profile002.ap"
prof.write_text("\n".join(out) + "\n", encoding="utf-16")
print(f"[+] profile002.ap: {len(sel)} переменных -> {prof}")

try:
    ET.fromstring("\n".join(out))
    print("[+] XML валиден")
except Exception as ex:
    print("[-] XML error:", ex)

# структура GAU — сводка для отчёта
with open(OUTDIR / "gaugehdr_count.txt", "w") as f:
    gh = [s for s in map_syms if re.match(r"\?gaugehdr_\w+@@3UGAUGEHDR@@A", s["name"])]
    f.write(f"gaugehdr symbols in map: {len(gh)}\n")
    for s in sorted(gh, key=lambda x: x["rva"]):
        f.write(f"0x{s['rva']:06X}  {s['name']}\n")
