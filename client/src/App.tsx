import { useRef, useState } from "react";
import "./App.css";

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
  const [screenSize, _setScreenSize] = useState({ width: 1920, height: 1080 });
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const [inputs, setInputs] = useState<string>("");

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
