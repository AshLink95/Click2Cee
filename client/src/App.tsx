import { useEffect, useRef, useState } from "react";
import type { KeyboardEvent, MouseEvent, WheelEvent } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./App.css";

function codecFromSps(b: Uint8Array): string | null {
  for (let i = 0; i + 7 < b.length; i++) {
    if (b[i] === 0 && b[i + 1] === 0 && b[i + 2] === 1 && (b[i + 3] & 0x1f) === 7) {
      const hex = [b[i + 4], b[i + 5], b[i + 6]]
        .map((x) => x.toString(16).padStart(2, "0"))
        .join("");
      return `avc1.${hex}`;
    }
  }
  return null;
}

function btnOf(buttons: number): number {
  if (buttons & 1)  return 1; // left
  if (buttons & 2)  return 2; // right
  if (buttons & 4)  return 3; // middle
  if (buttons & 8)  return 4; // pageDown
  if (buttons & 16) return 5; // pageUp
  return 0;                   // hover
}
function btnOfWheel(e: WheelEvent<HTMLCanvasElement>): number {
  let btn;
  if (e.deltaY === 0) btn = 0;      // no scroll
  else if (e.deltaY < 0) btn = 100; // scroll up
  else btn = 101;                   // scroll down
  return btn;
}

function click(e: MouseEvent, btn: number) {
  const r = e.currentTarget.getBoundingClientRect();
  const norm = (v: number, min: number, span: number) =>
    span > 0 ? Math.max(0, Math.min(65535, Math.round(((v - min) / span) * 65535))) : 0;

  return {
    x: norm(e.clientX, r.left, r.width),
    y: norm(e.clientY, r.top, r.height),
    btn,
  };
}

/// Only the code is kept, never the event: it is what goes on the wire, and a
/// synthetic event is not ours to hold past its handler anyway.
function key(a: Array<string>, e: KeyboardEvent, down: boolean) {
  const i = a.indexOf(e.code);

  if (down) {
    if (i !== -1) return a;
    return [...a, e.code];
  }
  if (i === -1) return a;

  return a.filter(c => c !== e.code);
}

export default function App() {
  const [screenSize, setScreenSize] = useState({ width: 1920, height: 1080 });
  const [inputs, setInputs] = useState<Array<string>>([]);
  const [mouse, setMouse] = useState({ x: 0, y: 0, btn: 0 });
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const heldRef = useRef<Array<string>>([]);
  const mouseRef = useRef<{ x: number, y: number, btn: number }>({ x: 0, y: 0, btn: 0 });
  const wheelRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const handleInput = (
    m: MouseEvent<HTMLCanvasElement> | null, mbt: number | null,
    e: KeyboardEvent | null, pressed: boolean | null
  ) => {
    let keys = heldRef.current;
    let { x, y, btn } = mouseRef.current;
    if (mbt !== null) {
      ({ x, y, btn } = m !== null ? click(m, mbt) : { ...mouseRef.current, btn: mbt });
      mouseRef.current = { x, y, btn };
      setMouse(mouseRef.current);
    }
    if (e !== null && pressed !== null) {
      e.preventDefault();
      keys = key(heldRef.current, e, pressed);
      if (keys === heldRef.current) return;
      heldRef.current = keys;
    }
    invoke("snd_input", { keys, x, y, btn })
      .catch(err => console.error("snd_input", err));
    setInputs(keys);
  };

  const handleBlur = () => {
    if (heldRef.current.length === 0) return;
    heldRef.current = [];
    setInputs([]);
    const { x, y, btn } = mouseRef.current;
    invoke("snd_input", { keys: [], x, y, btn })
      .catch(err => console.error("snd_input", err));
  };

  const handleWheel = (e: WheelEvent<HTMLCanvasElement>) => {
    const btn = btnOfWheel(e);
    handleInput(e, btn, null, null);
    if (wheelRef.current !== null) clearTimeout(wheelRef.current);
    wheelRef.current = setTimeout(() => {
      wheelRef.current = null;
      handleInput(null, 0, null, null);
    }, 150);
  };

  useEffect(() => {
    let stop = false;
    let decoder: VideoDecoder | null = null;

    (async () => {
      while (!stop) {
        // one round trip per frame. rcv_udp blocks until a frame is whole.
        let bytes: Uint8Array;
        try {
          bytes = new Uint8Array(await invoke<ArrayBuffer>("rcv_udp"));
        } catch (e) {
          console.error("rcv_udp", e);
          await new Promise((r) => setTimeout(r, 200));
          continue;
        }

        const codec = codecFromSps(bytes);

        // the decoder can only open on a keyframe, so drop everything until one
        if (!decoder) {
          if (!codec) continue;
          decoder = new VideoDecoder({
            output: (frame) => {
              const c = canvasRef.current;
              if (c) {
                if (c.width !== frame.displayWidth || c.height !== frame.displayHeight)
                  setScreenSize({ width: frame.displayWidth, height: frame.displayHeight });
                c.getContext("2d")?.drawImage(frame, 0, 0);
              }
              frame.close(); // not optional, leaks gpu memory otherwise
            },
            error: (e) => console.error("decode", e),
          });
          // no `description` field: that is what tells WebCodecs this is annex-B
          decoder.configure({ codec, optimizeForLatency: true });
        }

        decoder.decode(
          new EncodedVideoChunk({
            type: codec ? "key" : "delta",
            timestamp: performance.now() * 1000, // microseconds
            data: bytes,
          })
        );
      }
    })();

    return () => {
      stop = true;
      decoder?.close();
    };
  }, []);

  return (
    <main className="container">
      <div className="screen-wrap">
        <canvas
          ref={canvasRef}
          width={screenSize.width}
          height={screenSize.height}
          className="screen"
          onContextMenu={(e) => e.preventDefault()}
          onMouseUp={(e) => handleInput(e, 0, null, null)}
          onMouseDown={(e) => handleInput(e, btnOf(e.buttons), null, null)}
          onMouseMove={(e) => handleInput(e, btnOf(e.buttons), null, null)}
          onWheel={handleWheel}
          tabIndex={0}
          onKeyDown={(e) => handleInput(null, null, e, true)}
          onKeyUp={(e) => handleInput(null, null, e, false)}
          onBlur={(_) => handleBlur()}
        />
      </div>

      <div className="mouse-hint">
        <strong>Mouse: </strong>
          {mouse.x}x{mouse.y}:{mouse.btn}
      </div>
      <div className="input-hint">
        <strong>Inputs held: </strong>
          {inputs.join(" +\n")}
      </div>
    </main>
  );
}
