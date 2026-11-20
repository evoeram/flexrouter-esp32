# Запись WS-монитора пакетов платы в файл: python ws_capture.py <ip> <seconds> <out>
# Ловит все пакеты ARCC (rx - от роутера, tx - от платы) с расшифровкой.
import asyncio, sys, json
import websockets

ip, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]

async def main():
    lines = []
    async with websockets.connect(f"ws://{ip}/ws") as ws:
        t0 = asyncio.get_event_loop().time()
        while asyncio.get_event_loop().time() - t0 < secs:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=secs)
            except asyncio.TimeoutError:
                break
            try:
                d = json.loads(msg)
            except Exception:
                continue
            if d.get("t") != "pkt":
                continue
            dec = d.get("mod", "?")
            if dec == "button":
                dec = f"button mid? ctl={d.get('ctl')} {'НАЖАТА' if d.get('on') else 'отпущена'}"
            elif dec == "binary":
                dec = f"binary маска={d.get('mask')}"
            elif dec == "encoder":
                dec = f"encoder ctl={d.get('ctl')} {'+1' if d.get('cw') else '-1'}"
            elif dec == "axis":
                dec = f"axis {d.get('vals')}"
            elif dec == "lamp":
                dec = f"LAMP ctl={d.get('ctl')} {'ВКЛ' if d.get('on') else 'выкл'}"
            elif dec == "indicator":
                dec = f"indicator \"{d.get('text')}\""
            lines.append(f"{d.get('dir').upper()} {d.get('hex')} | {dec}")
    with open(out, "w") as f:
        f.write("\n".join(lines))
    print(f"захвачено {len(lines)} пакетов -> {out}")

asyncio.run(main())
