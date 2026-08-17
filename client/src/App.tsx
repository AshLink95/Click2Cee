import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./App.css";

// Annex-B: scan for a start code whose NAL type is 7 (SPS). The three bytes
// after it are profile_idc, constraint flags and level_idc, which spell the
// WebCodecs codec string. Finding one also means this frame is a keyframe.
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

function mods(e: { ctrlKey: boolean; shiftKey: boolean; altKey: boolean; metaKey: boolean; }) {
  const held = [
    e.ctrlKey && "Ctrl",
    e.shiftKey && "Shift",
    e.altKey && "Alt",
    e.metaKey && "Meta",
  ].filter(Boolean);
  return held.length ? held.join("+") + "+" : "";
}

export default function App() {
  const [screenSize, setScreenSize] = useState({ width: 1920, height: 1080 });
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const [inputs, setInputs] = useState<string>("");

  useEffect(() => {
    let stop = false;
    let decoder: VideoDecoder | null = null;

    (async () => {
      while (!stop) {
        // one round trip per frame. rcv_udp blocks until a frame is whole.
        let bytes: Uint8Array;
        try {
          bytes = new Uint8Array(await invoke<number[]>("rcv_udp"));
        } catch (e) {
          console.error("rcv_udp", e);
          break;
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
      {/* THE SCREEN */}
      {/* Wrapper keeps the 16:9 box centered. */}
      <div className="screen-wrap">
        <canvas
          ref={canvasRef}
          // width/height attrs = the pixel buffer (what you draw into).
          width={screenSize.width}
          height={screenSize.height}
          className="screen"
          onContextMenu={(e) => e.preventDefault()}
          onMouseDown={(e) => setInputs(`${e.button} @ ${e.clientX},${e.clientY}`)}
          onMouseMove={(e) => setInputs(`${e.clientX},${e.clientY}`)}
          // tabIndex makes the canvas focusable so it can catch key presses.
          tabIndex={0}
          onKeyDown={(e) => {
            e.preventDefault();
            setInputs(`${mods(e)}${e.key}`)
          }}
        />
      </div>

      <div className="input-record">
        <strong>Last input sent: </strong>
          {inputs}
      </div>
    </main>
  );
}
