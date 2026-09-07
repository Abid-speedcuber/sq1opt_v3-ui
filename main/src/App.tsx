import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { loadSettings, saveSettings, loadFavorites, saveFavorites, writeOffloadedChunk, readOffloadedChunk, removeOffloadedChunk, clearOffloadedSolutions, isNativePlatform, writeBatchInputChunk, readBatchInputChunk, clearBatchInputChunks } from "./storage";
import { clearTableBlobs } from "./tableStore";
import { clearAllTables } from "./diskSpace";
import {
  CubeState, Modal as ModalType, FavoriteBin, Solution, OutputLine, RatingResult, TwoGenStatus,
  DisplaySolution, DropdownProps, CubeActions, TauriGlobal,
  twistable, getLayerR, getParityOdd, isGoodSquares, inCubeshape, doMove, tauri, validDepths, solverFlags,
  positionString, rawPosition, parsePosition, invertScramble, addCommas, applyNumericAlgorithm,
  abidify, abidSpacing, notationStyle, isKarnMode, OutputMode, OUTPUT_MODES, injectSliceIndicator, lineAlg, lineWithoutBracket, parseSolutionCounts,
  ratingScore, ratingDebugAnnotation, ratingSliceStart, solutionErgo, medianNormalize, normalizeLine, tooltips, DEFAULT_WEIGHTS,
} from "./utils";
import { Modal } from './components/Modal';
import { Icon } from './components/Icon';
import { generateRemapCandidates, computeRotationClassKey } from './utils/remap';
import { DiskSpaceModal } from './components/DiskSpaceModal';
import { WeightsModal } from './components/WeightsModal';
import { t, tList, LangCode, getLang, setLang } from './i18n';

// Sets --tt-x/--tt-y from the hovered element's rect so the fixed-position
// tooltip in styles.css can anchor to it without being clipped by scroll ancestors.
function positionTooltip(event: React.MouseEvent<HTMLElement>) {
  const r = event.currentTarget.getBoundingClientRect();
  event.currentTarget.style.setProperty("--tt-x", `${r.left + r.width / 2}px`);
  event.currentTarget.style.setProperty("--tt-y", `${r.top}px`);
}

const PAGE_SIZE_OPTIONS = [250, 500, 1000, 2000, 5000, 8000, 10000, 15000, 20000];
const OFFLOAD_CHUNK = 5000;
// Batch input stays editable inline (in the textarea) up to this many lines;
// pasting more migrates it into storage ("Pasted N lines") so a million-row
// paste never has to live in memory as one giant string.
const BATCH_INLINE_MAX_LINES = 50000;
const BATCH_INPUT_CHUNK_SIZE = 50000;
const BATCH_PROGRESS_EVERY = 10;
// Global rotation-class grouping partitions classes into hash buckets and
// groups one bucket in RAM at a time; this caps that bucket's memory use at
// roughly one input chunk's worth of lines.
const BATCH_GROUP_BUCKET_LINES = BATCH_INPUT_CHUNK_SIZE;
// FNV-1a (32-bit) over the rotation-class key string.  Deterministic on every
// platform so the same key always lands in the same bucket.
function fnv1a(str: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}
const LEGACY_METRIC: Record<string, string> = { Slice: "es", Move: "move", Angle: "ea", es: "es", move: "move", ea: "ea" };
const LEGACY_TWO: Record<string, string> = { None: "none", "Pseudo 2 Gen": "p2g", "2 Gen": "2g", none: "none", p2g: "p2g", "2g": "2g" };
const LEGACY_ANGLE: Record<string, string> = { None: "none", Both: "nb", Top: "nu", Bottom: "nd", none: "none", nb: "nb", nu: "nu", nd: "nd" };
const LEGACY_NORMALIZE: Record<string, string> = { None: "none", Both: "both", PreABF: "pre", PostABF: "post", none: "none", both: "both", pre: "pre", post: "post" };
const TABLE_COL_WIDTHS = {
  wide: { hash: 72, num: 48, ergo: 52, minSol: 150 },
  compact: { hash: 34, num: 42, ergo: 50, minSol: 88 },
};
const computeTableCols = (
  width: number, metric: string, showErgo: boolean, compact: boolean,
): { hash: boolean; angle: boolean; move: boolean; slices: boolean; ergo: boolean; template: string } => {
  const sizes = compact ? TABLE_COL_WIDTHS.compact : TABLE_COL_WIDTHS.wide;
  const w = (key: string) => (key === "hash" ? sizes.hash : key === "ergo" ? sizes.ergo : sizes.num);
  const want = new Set<string>(["hash"]);
  if (metric === "ea") want.add("angle");
  if (metric !== "es") want.add("move");
  want.add("slices");
  if (showErgo) want.add("ergo");
  const keepPriority = ["ergo", "angle", "move", "slices", "hash"];
  const cols = [...want];
  const total = (list: string[]) => list.reduce((sum, key) => sum + w(key), 0) + sizes.minSol;
  while (cols.length > 1 && total(cols) > width) {
    const drop = keepPriority.slice().reverse().find((key) => cols.includes(key));
    if (!drop) break;
    cols.splice(cols.indexOf(drop), 1);
  }
  const template = ["hash", "solution", "angle", "move", "slices", "ergo"]
    .map((key) => (key === "solution" ? "minmax(0, 1fr)" : cols.includes(key) ? `${w(key)}px` : null))
    .filter((part): part is string => !!part)
    .join(" ");
  return {
    hash: cols.includes("hash"),
    angle: cols.includes("angle"),
    move: cols.includes("move"),
    slices: cols.includes("slices"),
    ergo: cols.includes("ergo"),
    template,
  };
};
const computeTotalPages = (total: number, pageSize: number) => {
  const raw = Math.max(1, Math.ceil(total / pageSize));
  const remainder = total % pageSize;
  return remainder > 0 && remainder < pageSize * 0.1 ? Math.max(1, raw - 1) : raw;
};
const readBreakpoints = () => {
  const cs = getComputedStyle(document.documentElement);
  const read = (name: string) => {
    const value = parseFloat(cs.getPropertyValue(name));
    return Number.isFinite(value) ? value : 0;
  };
  return {
    tall: read("--bp-tall"),
    wide: read("--bp-wide"),
    semi: read("--bp-semi"),
    narrow: read("--bp-narrow"),
    panel: read("--bp-panel"),
    panelTiny: read("--bp-panel-tiny"),
  };
};
const solved = [
  0, 0, 8, 1, 1, 9, 2, 2, 10, 3, 3, 11, 12, 4, 4, 13, 5, 5, 14, 6, 6, 15, 7, 7,
];
const side = [
  [2, 3],
  [3, 4],
  [4, 5],
  [5, 2],
  [2, 5],
  [5, 4],
  [4, 3],
  [3, 2],
  [3],
  [4],
  [5],
  [2],
  [2],
  [5],
  [4],
  [3],
];
const colors = ["#333", "#fff", "#f00", "#00f", "#ff8600", "#0f0", "#888"];

function OptionDropdown({ id, label, title, value, options, disabled, highlight, open, setOpen, onChange }: DropdownProps) {
  const current = options.find((option) => option.value === value)?.label ?? value;
  const buttonRef = useRef<HTMLButtonElement>(null);
  const [activeIndex, setActiveIndex] = useState(() => Math.max(0, options.findIndex((option) => option.value === value)));
  useEffect(() => {
    if (!open) return;
    setActiveIndex(Math.max(0, options.findIndex((option) => option.value === value)));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);
  const commit = (index: number) => {
    const option = options[index];
    if (option) onChange(option.value);
    setOpen(null);
  };
  const handleKeyDown = (event: React.KeyboardEvent<HTMLButtonElement>) => {
    if (disabled) return;
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      if (!open) { setOpen(id); return; }
      const delta = event.key === "ArrowDown" ? 1 : -1;
      setActiveIndex((index) => (index + delta + options.length) % options.length);
    } else if (event.key === "Home" && open) {
      event.preventDefault();
      setActiveIndex(0);
    } else if (event.key === "End" && open) {
      event.preventDefault();
      setActiveIndex(options.length - 1);
    } else if ((event.key === "Enter" || event.key === " ") && open) {
      event.preventDefault();
      commit(activeIndex);
    } else if (event.key === "Escape" && open) {
      event.preventDefault();
      event.stopPropagation();
      setOpen(null);
      event.currentTarget.blur();
    }
  };
  return (
    <label className="option-dropdown-label" title={title}>
      {highlight && value !== "none" ? <span style={{ color: "white", fontWeight: "bold", WebkitTextStroke: "0.2px white" }}>{label}</span> : label}
      <div className="option-dropdown">
        <button
          ref={buttonRef}
          type="button"
          disabled={disabled}
          aria-haspopup="listbox"
          aria-expanded={open}
          aria-activedescendant={open ? `${id}-option-${activeIndex}` : undefined}
          onMouseDown={(event) => event.preventDefault()}
          onClick={(event) => {
            if (disabled) return;
            const next = open ? null : id;
            setOpen(next);
            if (next) event.currentTarget.focus();
          }}
          onKeyDown={handleKeyDown}
        >
          <span>{current}</span>
          <span className="option-dropdown-arrow"><Icon name="chevronDown" size={10} /></span>
        </button>
        {open && !disabled && (
          <div className="option-dropdown-menu" role="listbox">
            {options.map((option, index) => (
              <button
                key={option.value}
                id={`${id}-option-${index}`}
                type="button"
                role="option"
                aria-selected={option.value === value}
                className={(option.value === value ? "selected" : "") + (index === activeIndex ? " active" : "")}
                onMouseEnter={() => setActiveIndex(index)}
                onMouseDown={(event) => {
                  event.preventDefault();
                  onChange(option.value);
                  setOpen(null);
                  buttonRef.current?.blur();
                }}
              >
                {option.label}
              </button>
            ))}
          </div>
        )}
      </div>
    </label>
  );
}


function Cube({
  onChange,
  actionsRef,
  onOptions,
  shortcutsBlocked,
}: {
  onChange: (s: CubeState, action?: string) => void;
  actionsRef: React.MutableRefObject<CubeActions | undefined>;
  onOptions: () => void;
  shortcutsBlocked?: boolean;
}) {
  const [s, setS] = useState<CubeState>({
    position: [...solved],
    partial: Array(24).fill(0),
    middle: 1,
    middlePartial: 0,
  });
  const canvas = useRef<HTMLCanvasElement>(null);
  const [hovered, setHovered] = useState(-1);
  const [hoverProgress, setHoverProgress] = useState<Record<number, number>>(
    {},
  );
  const [selected, setSelected] = useState(-1);
  const update = (n: CubeState, action = "edit") => {
    setS(n);
    onChange(n, action);
  };
  const polar = (
    cx: number,
    cy: number,
    a: number,
    r: number,
  ): [number, number] => [
    cx + Math.cos((a * Math.PI) / 180) * r,
    cy - Math.sin((a * Math.PI) / 180) * r,
  ];
  const hulls = (
    start: number,
    end: number,
    cy: number,
    startAngle: number,
  ) => {
    const found: { piece: number; pts: number[][] }[] = [];
    let angle = startAngle;
    for (let i = start; i < end; ) {
      const x = s.position[i],
        corner = x < 8,
        pts = corner
          ? [
              [150, cy],
              polar(150, cy, angle, 75),
              polar(150, cy, angle - 30, 75 / 0.73205080756),
              polar(150, cy, angle - 60, 75),
            ]
          : [
              [150, cy],
              polar(150, cy, angle, 75),
              polar(150, cy, angle - 30, 75),
            ];
      found.push({ piece: i, pts });
      if (corner) {
        i++;
        angle -= 60;
      } else angle -= 30;
      i++;
    }
    return found;
  };
  const hit = (x: number, y: number) => {
    if (y < 235) {
      for (const h of hulls(0, 12, 125, -105)) {
        const c = canvas.current!.getContext("2d")!;
        c.beginPath();
        c.moveTo(h.pts[0][0], h.pts[0][1]);
        h.pts.slice(1).forEach((p) => c.lineTo(p[0], p[1]));
        c.closePath();
        if (c.isPointInPath(x, y)) return h.piece;
      }
      return -1;
    }
    if (y < 265) return x >= 77.25 && x <= 222.75 ? -2 : -1;
    for (const h of hulls(12, 24, 375, 105)) {
      const c = canvas.current!.getContext("2d")!;
      c.beginPath();
      c.moveTo(h.pts[0][0], h.pts[0][1]);
      h.pts.slice(1).forEach((p) => c.lineTo(p[0], p[1]));
      c.closePath();
      if (c.isPointInPath(x, y)) return h.piece;
    }
    return -1;
  };
  const draw = () => {
    const c = canvas.current?.getContext("2d");
    if (!c) return;
    c.clearRect(0, 0, 300, 500);
    const poly = (pts: number[][], fill: string, hover = 0) => {
      c.beginPath();
      c.moveTo(pts[0][0], pts[0][1]);
      pts.slice(1).forEach((p) => c.lineTo(p[0], p[1]));
      c.closePath();
      c.fillStyle = fill;
      c.fill();
      c.strokeStyle = "#000";
      c.lineWidth = 1;
      c.stroke();
      if (hover > 0) {
        const eased = hover * hover * (3 - 2 * hover);
        c.fillStyle = `rgba(255,255,255,${eased * (60 / 255)})`;
        c.fill();
      }
    };
    const layer = (
      start: number,
      end: number,
      cy: number,
      startAngle: number,
    ) => {
      let angle = startAngle;
      let sel: number[][] | undefined;
      for (let i = start; i < end; ) {
        const x = s.position[i],
          corner = x < 8,
          h = hoverProgress[i] || 0,
          p1 = polar(150, cy, angle, 50),
          p2 = polar(150, cy, angle - 30, 50 / (corner ? 0.73205080756 : 1)),
          p3 = corner ? polar(150, cy, angle - 60, 50) : p2,
          q1 = polar(150, cy, angle, 75),
          q2 = polar(150, cy, angle - 30, 75 / (corner ? 0.73205080756 : 1)),
          q3 = corner ? polar(150, cy, angle - 60, 75) : q2;
        const faceColor = corner
          ? colors[x < 4 ? 0 : 1]
          : colors[x < 12 ? 0 : 1];
        poly(
          corner ? [[150, cy], p1, p2, p3] : [[150, cy], p1, p2],
          s.partial[i] > 1 ? colors[6] : faceColor,
          h,
        );
        poly(
          [p1, q1, q2, p2],
          s.partial[i] > 0 ? colors[6] : colors[side[x][0]],
          h,
        );
        if (corner)
          poly(
            [p2, q2, q3, p3],
            s.partial[i] > 0 ? colors[6] : colors[side[x][1]],
            h,
          );
        if (selected === i)
          sel = corner ? [[150, cy], q1, q2, q3] : [[150, cy], q1, q2];
        if (corner) {
          i++;
          angle -= 60;
        } else angle -= 30;
        i++;
      }
      if (sel) {
        c.beginPath();
        c.moveTo(sel[0][0], sel[0][1]);
        sel.slice(1).forEach((p) => c.lineTo(p[0], p[1]));
        c.closePath();
        c.strokeStyle = "#ffff00";
        c.lineWidth = 3;
        c.stroke();
      }
    };
    layer(0, 12, 125, -105);
    layer(12, 24, 375, 105);
    const eqHover = hoverProgress[-2] || 0;
    poly(
      [
        [77.25, 235],
        [129, 235],
        [129, 265],
        [77.25, 265],
      ],
      colors[2],
      eqHover,
    );
    poly(
      [
        [129, 235],
        [s.middle === -1 ? 171 : 222.75, 235],
        [s.middle === -1 ? 171 : 222.75, 265],
        [129, 265],
      ],
      s.middle === 0 ? colors[6] : colors[s.middle === 1 ? 2 : 4],
      eqHover,
    );
  };
  useEffect(() => {
    draw();
  }, [s, hoverProgress, selected]);
  useEffect(() => {
    let frame = 0, stopped = false, frames = 0;
    const tick = () => {
      setHoverProgress((old) => {
        const next: { [key: number]: number } = { ...old };
        let changed = false;
        for (const key of new Set([...Object.keys(next).map(Number), hovered])) {
          const target = key === hovered ? 1 : 0, value = next[key] || 0;
          const valueNext = target ? Math.min(1, value + 0.08) : Math.max(0, value - 0.08);
          if (valueNext !== value) { next[key] = valueNext; changed = true; }
        }
        return changed ? next : old;
      });
      if (!stopped && ++frames < 16) frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => { stopped = true; cancelAnimationFrame(frame); };
  }, [hovered]);
  const invoke = (key: keyof CubeActions, actionOverride?: string) => {
    if (key === "reset") {
      setSelected(-1);
      update({
        position: [...solved],
        partial: Array(24).fill(0),
        middle: 1,
        middlePartial: 0,
      }, actionOverride ?? "reset");
      return;
    }
    setSelected(-1);
    update(doMove(s, key), key);
  };
  actionsRef.current = {
    u: () => invoke("u"),
    up: () => invoke("up"),
    d: () => invoke("d"),
    dp: () => invoke("dp"),
    slice: () => invoke("slice"),
    reset: () => invoke("reset"),
    set: (next) => {
      setSelected(-1);
      update(next, "set");
    },
  };
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      if (shortcutsBlocked) return;
      const target = e.target as HTMLElement | null;
      if (target?.matches("input, textarea, select, [contenteditable=true]")) return;
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const map: Record<string, keyof CubeActions> = {
        i: "slice",
        k: "slice",
        j: "u",
        f: "up",
        s: "d",
        l: "dp",
        escape: "reset",
      };
      // Double-turn shortcuts: each equivalent to pressing its base key twice.
      const doubleMap: Record<string, keyof CubeActions> = {
        g: "up", // G = F twice
        h: "u",  // H = J twice
        o: "dp", // O = L twice
        w: "d",  // W = S twice
      };
      const pressed = e.key.toLowerCase();
      const action = map[pressed];
      if (action) {
        e.preventDefault();
        invoke(action, action === "reset" ? "escape" : undefined);
        return;
      }
      const doubleAction = doubleMap[pressed];
      if (doubleAction) {
        e.preventDefault();
        const mid = doMove(s, doubleAction);
        setSelected(-1);
        update(mid, doubleAction);
        update(doMove(mid, doubleAction), doubleAction);
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, [s, shortcutsBlocked]);
  const pointer = (e: React.MouseEvent) => {
    const r = canvas.current!.getBoundingClientRect();
    return [
      ((e.clientX - r.left) * 300) / r.width,
      ((e.clientY - r.top) * 500) / r.height,
    ];
  };
  const click = (e: React.MouseEvent) => {
    const [x, y] = pointer(e),
      piece = hit(x, y);
    if (piece === -2) {
      const middle =
        e.button === 2
          ? s.middle === 1
            ? 0
            : s.middle === 0
              ? -1
              : 1
          : s.middle === 1
            ? -1
            : s.middle === -1
              ? 0
              : 1;
      update({ ...s, middle }, "edit");
      return;
    }
    if (piece < 0) return;
    if (e.button === 2) {
      const partial = [...s.partial];
      partial[piece] = (partial[piece] + 1) % 3;
      if (piece < 23 && s.position[piece] === s.position[piece + 1])
        partial[piece + 1] = partial[piece];
      update({ ...s, partial }, "edit");
      return;
    }
    if (selected < 0) {
      setSelected(piece);
      return;
    }
    const p = [...s.position],
      q = [...s.partial],
      selCorner = selected < 23 && p[selected] === p[selected + 1],
      pieCorner = piece < 23 && p[piece] === p[piece + 1];
    if (selCorner !== pieCorner) {
      setSelected(piece);
      return;
    }
    if (selected === piece) {
      setSelected(-1);
      return;
    }
    if (selCorner) {
      [p[selected + 1], p[piece + 1]] = [p[piece + 1], p[selected + 1]];
      [q[selected + 1], q[piece + 1]] = [q[piece + 1], q[selected + 1]];
    }
    [p[selected], p[piece]] = [p[piece], p[selected]];
    [q[selected], q[piece]] = [q[piece], q[selected]];
    setSelected(-1);
    update({ ...s, position: p, partial: q }, "edit");
  };
  return (
    <div className="cube-holder">
      <canvas
        ref={canvas}
        width={300}
        height={500}
        style={{
          cursor: hovered !== -1 ? "pointer" : "default",
        }}
        onMouseMove={(e) => {
          const [x, y] = pointer(e);
          setHovered(hit(x, y));
        }}
        onMouseLeave={() => setHovered(-1)}
        onMouseDown={click}
        onContextMenu={(e) => e.preventDefault()}
      />
      <button className="cube-reset" onClick={() => invoke("reset")}>
        {t('btn.resetCube')}
      </button>
      <button className="cube-options" onClick={onOptions}>
        {t('btn.options')}
      </button>
    </div>
  );
}

function Pill({
  label,
  items,
  value,
  set,
}: {
  label: string;
  items: string[];
  value: string;
  set: (v: string) => void;
}) {
  return (
    <div className="option-row">
      <span>{label}</span>
      <div className="pill">
        {items.map((x) => (
          <button
            className={x === value ? "active" : ""}
            key={x}
            onClick={() => set(x)}
          >
            {x}
          </button>
        ))}
      </div>
    </div>
  );
}
export default function App() {
  const cubeActions = useRef<CubeActions | undefined>(undefined);
  const modeControlRef = useRef<HTMLDivElement | null>(null);
  const stateRef = useRef<CubeState>({ position: [...solved], partial: Array(24).fill(0), middle: 1, middlePartial: 0 });
  const ignoreHistory = useRef(false), seenRaw = useRef(new Set<string>()), seenDisplay = useRef(new Set<string>());
  const lastSolvePosition = useRef("A1B2C3D45E6F7G8H-");
  const lastSolveCubeShape = useRef(false);
  const solutionsRef = useRef<Solution[]>([]);
  const outputLinesRef = useRef<OutputLine[]>([]);
  const followTerminalRef = useRef(true);
  const firstSolutionAt = useRef(0);
  const solveStartTimeRef = useRef(0);
  const solveStopTimeRef = useRef(0);
  const debugStatsRef = useRef<{ solutionTimestamps: number[] }>({ solutionTimestamps: [] });
  const rateHistoryRef = useRef<{ t: number; sol: number; node: number }[]>([]);
  const progressNodesRef = useRef(0);
  const progressRateRef = useRef(0);
  const lastProgressNodesRef = useRef(0);
  const lastProgressAtRef = useRef(0);
  const lineQueue = useRef<Promise<void>>(Promise.resolve());
  const renderFrame = useRef<number | undefined>(undefined);
  const runningRef = useRef(false);
  const solveRunId = useRef(0);
  const preIgnoreMiddle = useRef(1);
  const slicePending = useRef<string[]>([]), sliceTimer = useRef<number | undefined>(undefined);
  const stopped = useRef(false);
  const settingsReady = useRef(false);
  const favoritesReady = useRef(false);
  const favShadeStartRef = useRef<EventTarget | null>(null);
  const favShadeEndRef = useRef<EventTarget | null>(null);
  const toastTimerRef = useRef<number | undefined>(undefined);
  const [menu, setMenu] = useState(false),
    [lang, setLangState] = useState<LangCode>(getLang()),
    [toast, setToast] = useState<string | null>(null),
    [modal, setModal] = useState<ModalType>(null),
    [modalStack, setModalStack] = useState<ModalType[]>([]),
    [modeMenu, setModeMenu] = useState(false),
    [openDropdown, setOpenDropdown] = useState<string | null>(null),
    [input, setInput] = useState(""),
    [mode, setMode] = useState("SCRAMBLE"),
    [cubeState, setCubeState] = useState<CubeState>({ position: [...solved], partial: Array(24).fill(0), middle: 1, middlePartial: 0 }),
    [metric, setMetric] = useState("es"),
    [two, setTwo] = useState("none"),
    [angle, setAngle] = useState("none"),
    [normalize, setNormalize] = useState("none"),
    [all, setAll] = useState(true),
    [generator, setGenerator] = useState(false),
    [cubeShapeMemory, setCubeShapeMemory] = useState(false),
    [ignoreMiddle, setIgnoreMiddle] = useState(false),
    [outputMode, setOutputMode] = useState<OutputMode>("cskarn"),
    [outputLines, setOutputLines] = useState<OutputLine[]>([]),
    [statusLines, setStatusLines] = useState<string[]>([]),
    [solutions, setSolutions] = useState<Solution[]>([]),
    [runCubeShape, setRunCubeShape] = useState(false),
    [running, setRunning] = useState(false),
    [suboptimal, setSuboptimal] = useState(0),
    [depths, setDepths] = useState(""),
    [maxX, setMaxX] = useState(false),
    [maxXValue, setMaxXValue] = useState(3),
    [maxY, setMaxY] = useState(false),
    [maxYValue, setMaxYValue] = useState(3),
    [maxTotal, setMaxTotal] = useState(false),
    [maxTotalValue, setMaxTotalValue] = useState(6),
    [inputError, setInputError] = useState(""),
    [undo, setUndo] = useState<string[]>([]), [redo, setRedo] = useState<string[]>([]),
    [tableView, setTableView] = useState(false), [expanded, setExpanded] = useState(false),
    [mobileOptionsOpen, setMobileOptionsOpen] = useState(false), [mobileOutputOpen, setMobileOutputOpen] = useState(false),
    [tableWidth, setTableWidth] = useState(0),
    [zoom, setZoom] = useState(1),
    [abidNotation, setAbidNotation] = useState(false),
    [ignoreTransforms, setIgnoreTransforms] = useState(false), [debugOutput, setDebugOutput] = useState(false),
    [pageSize, setPageSize] = useState(1000), [showAll, setShowAll] = useState(false),
    [page, setPage] = useState(0),
    [pageInput, setPageInput] = useState("1");
  const [useLessRam, _setUseLessRam] = useState(false);
  const karn = isKarnMode(outputMode);
  const smartKarn = outputMode === "cskarn";
  const [offloadedTotal, setOffloadedTotal] = useState(0);
  const [pendingTailCount, setPendingTailCount] = useState(0);
  const [isRestoring, setIsRestoring] = useState(false);
  const setUseLessRam = (value: boolean) => { useLessRamRef.current = value; _setUseLessRam(value); };
  const totalCount = solutions.length + offloadedTotal + pendingTailCount;
  const debugOutputRef = useRef(false);
  const deleteTablesOnQuitRef = useRef(!isNativePlatform());
  const [deleteTablesOnQuit, _setDeleteTablesOnQuit] = useState(!isNativePlatform());
  const deleteTablesOnQuitV2 = deleteTablesOnQuit;
  const setDeleteTablesOnQuit = (value: boolean) => {
    deleteTablesOnQuitRef.current = value;
    _setDeleteTablesOnQuit(value);
    if (value && !isNativePlatform()) void clearTableBlobs();
  };
  const [diskOpen, setDiskOpen] = useState(false);
  const [weightsOpen, setWeightsOpen] = useState(false);
  const [weightOverrides, setWeightOverrides] = useState<Partial<{ w1: number; w2: number; w3: number; w4: number }>>({});
  const [moveValueOverrides, setMoveValueOverrides] = useState<Record<string, number>>({});
  const [showAllConfirm, setShowAllConfirm] = useState(false);
  const [favoritesOpen, setFavoritesOpen] = useState(false),
    [favorites, setFavorites] = useState<Record<string, FavoriteBin>>({}),
    [pendingDeletes, setPendingDeletes] = useState<Record<string, number>>({});
  const [researchMode, setResearchMode] = useState(false);
  const [batchInput, setBatchInput] = useState("");
  const [batchTarget, setBatchTarget] = useState("");
  const [batchStored, _setBatchStored] = useState(false);
  const batchStoredRef = useRef(false);
  const setBatchStored = (value: boolean) => { batchStoredRef.current = value; _setBatchStored(value); };
  const [batchStoredLines, setBatchStoredLines] = useState(0);
  const batchStoredLinesRef = useRef(0);
  const setBatchStoredLinesState = (value: number) => { batchStoredLinesRef.current = value; setBatchStoredLines(value); };
  const batchInputChunkCountRef = useRef(0);
  const batchSolvedRef = useRef(0);
  const [batchSolved, setBatchSolved] = useState(0);
  const batchIndexRef = useRef(0);
  const batchProgressFrameRef = useRef<number | undefined>(undefined);
  const forceOffloadRef = useRef(false);
  const [batchOffload, setBatchOffload] = useState(false);
  const [batchRunning, setBatchRunning] = useState(false);
  const [batchStatsText, setBatchStatsText] = useState<string | null>(null);
  const [batchDragOver, setBatchDragOver] = useState(false);
  const batchStopRef = useRef(false);
  const batchQueueRef = useRef<Promise<void>>(Promise.resolve());
  const batchMetricKey: "moves" | "angle" | "slices" = metric === "move" ? "moves" : metric === "ea" ? "angle" : "slices";
  const batchMetricLabel = metric === "move" ? "moves" : metric === "ea" ? "angle" : "slices";
  const [favoritesClosing, setFavoritesClosing] = useState(false);
  const [favoritesOpening, setFavoritesOpening] = useState(false);
  const favModalRef = useRef<HTMLDivElement>(null);
  const favClosingOriginRef = useRef({ x: 50, y: 50 });
  const favOpeningOriginRef = useRef({ x: 50, y: 50 });
  const [contextMenu, setContextMenu] = useState<{ x: number; y: number; alg: string } | null>(null);
  const [stickyTooltip, setStickyTooltip] = useState<{ x: number; y: number; text: string; key: string } | null>(null);
  const stickyTooltipRef = useRef<HTMLDivElement>(null);
  // Sticky tooltip is a real element, so clamp its actual measured box to the
  // viewport (unlike the hover tooltip, which can only clamp its anchor point).
  useLayoutEffect(() => {
    const el = stickyTooltipRef.current;
    if (!stickyTooltip || !el) return;
    const margin = 8, gap = 6;
    const w = el.offsetWidth, h = el.offsetHeight;
    let left = Math.min(Math.max(stickyTooltip.x - w / 2, margin), window.innerWidth - w - margin);
    let top = stickyTooltip.y - h - gap;
    if (top < margin) top = stickyTooltip.y + gap; // no room above anchor: flip below
    top = Math.min(Math.max(top, margin), window.innerHeight - h - margin);
    el.style.left = `${left}px`;
    el.style.top = `${top}px`;
    el.style.visibility = "visible";
  }, [stickyTooltip]);
  const [twoGenStatus, setTwoGenStatus] = useState<TwoGenStatus>({ compatibility: 0, cornersTwo: false, cornersPseudo: false });
  const [followTerminal, setFollowTerminal] = useState(true);
  const [completedWhilePaused, setCompletedWhilePaused] = useState(false);
  const [tableBusyMessage, setTableBusyMessage] = useState("");
  const [tableBusyTick, setTableBusyTick] = useState(0);
  const [debugTick, setDebugTick] = useState(0);
  // Search/filter panel for narrowing the terminal & table down to matching algs.
  const [filterOpen, setFilterOpen] = useState(false);
  const [filterQuery, setFilterQuery] = useState("");
  const [filterMatchCase, setFilterMatchCase] = useState(true);
  const [filterRegexMode, setFilterRegexMode] = useState(false);
  const [filterNegate, setFilterNegate] = useState(false);
  const [filterResults, setFilterResults] = useState<Solution[] | null>(null);
  const [filterAppliedQuery, setFilterAppliedQuery] = useState("");
  const [filterSearching, setFilterSearching] = useState(false);
  const [filterInvalid, setFilterInvalid] = useState(false);
  const filterSearchIdRef = useRef(0);
  const filterInputRef = useRef<HTMLInputElement | null>(null);
  useEffect(() => {
    try {
      const storedCase = localStorage.getItem("sq1opt.filterMatchCase");
      if (storedCase !== null) setFilterMatchCase(storedCase === "1");
      const storedRegex = localStorage.getItem("sq1opt.filterRegex");
      if (storedRegex !== null) setFilterRegexMode(storedRegex === "1");
      const storedNegate = localStorage.getItem("sq1opt.filterNegate");
      if (storedNegate !== null) setFilterNegate(storedNegate === "1");
    } catch { /* localStorage unavailable */ }
  }, []);
  useEffect(() => {
    try { localStorage.setItem("sq1opt.filterMatchCase", filterMatchCase ? "1" : "0"); } catch { /* localStorage unavailable */ }
  }, [filterMatchCase]);
  useEffect(() => {
    try { localStorage.setItem("sq1opt.filterRegex", filterRegexMode ? "1" : "0"); } catch { /* localStorage unavailable */ }
  }, [filterRegexMode]);
  useEffect(() => {
    try { localStorage.setItem("sq1opt.filterNegate", filterNegate ? "1" : "0"); } catch { /* localStorage unavailable */ }
  }, [filterNegate]);
  useEffect(() => {
    if (filterOpen) requestAnimationFrame(() => filterInputRef.current?.focus());
  }, [filterOpen]);
  // Independent of filterOpen so hiding the overlay (✕) keeps the filter applied.
  const filterActive = filterAppliedQuery.trim() !== "" && filterResults !== null;
  const beginCloseFavorites = () => {
    if (favoritesClosing) return;
    const heartBtn = document.querySelector<HTMLElement>('.top-favorites-button');
    const modalEl = favModalRef.current;
    if (heartBtn && modalEl) {
      const hr = heartBtn.getBoundingClientRect();
      const mr = modalEl.getBoundingClientRect();
      favClosingOriginRef.current = {
        x: ((hr.left + hr.width / 2 - mr.left) / mr.width) * 100,
        y: ((hr.top + hr.height / 2 - mr.top) / mr.height) * 100,
      };
    }
    setFavoritesClosing(true);
  };
  const onFavCloseAnimEnd = (e: React.AnimationEvent) => {
    if (e.animationName !== "fav-modal-out") return;
    setFavoritesOpen(false);
    setFavoritesClosing(false);
    history.back();
  };
  const onFavOpenAnimEnd = (e: React.AnimationEvent) => {
    if (e.animationName !== "fav-modal-in") return;
    setFavoritesOpening(false);
  };
  useLayoutEffect(() => {
    if (!favoritesOpening || !favModalRef.current) return;
    const heartBtn = document.querySelector<HTMLElement>('.top-favorites-button');
    const modalEl = favModalRef.current;
    if (heartBtn && modalEl) {
      const hr = heartBtn.getBoundingClientRect();
      const mw = modalEl.offsetWidth, mh = modalEl.offsetHeight;
      const ml = (window.innerWidth - mw) / 2, mt = (window.innerHeight - mh) / 2;
      favOpeningOriginRef.current = {
        x: ((hr.left + hr.width / 2 - ml) / mw) * 100,
        y: ((hr.top + hr.height / 2 - mt) / mh) * 100,
      };
      modalEl.style.transformOrigin = `${favOpeningOriginRef.current.x}% ${favOpeningOriginRef.current.y}%`;
      modalEl.style.animationPlayState = 'running';
    }
  }, [favoritesOpening]);
  /* ============================================================================
   * TERMINAL / OUTPUT PANEL BEHAVIOR
   * ============================================================================
   *
   * The terminal is the output panel that shows solver log lines and solutions.
   * There are two views: terminal (raw text) and table (structured solutions).
   * Key refs/state:
   *   followTerminal / followTerminalRef  – whether auto-scroll is active
   *   completedWhilePaused               – solve finished while user had scrolled up
   *   isSwitchingViewRef                  – suppresses scroll during terminal↔table toggle
   *   terminalScrollPositionRef           – remembered scroll offset when switching views
   *   tableScrollPositionRef              – same, for table view
   *
   * ── SOLVE START (line ~1493) ──────────────────────────────────────────────
   *   Resets everything: followTerminal = true, tableView = false, scroll
   *   positions = 0, completedWhilePaused = false.
   *
   * ── DURING SOLVE (running = true) ─────────────────────────────────────────
   *   • New outputLines / statusLines / solutions arrive continuously.
   *   • The useEffect at line ~1230 fires on every content change. If
   *     followTerminal is true, it schedules a requestAnimationFrame to scroll
   *     the terminal to the bottom.
   *   • Scrolling UP (onWheel with deltaY < 0) sets followTerminal = false,
   *     which stops auto-scroll. A ⌄ button appears to re-enable it.
   *   • Scrolling up via scrollbar drag (no onWheel) is detected by
   *     handleTerminalScroll: it compares the new scrollTop with the previous
   *     one — only if scrollTop decreased (user actually scrolled up) AND the
   *     user is >50px above the bottom does it disable followTerminal. This
   *     avoids false triggers from content being added (scrollHeight grows
   *     but scrollTop stays the same). Re-enabling is only via the ⌄ button.
   *
   * ── SOLVE FINISHES (line ~1520) ──────────────────────────────────────────
   *   • If followTerminal was still true (user never scrolled away):
   *     → auto-switches to table view
   *     → shows busy messages while normalizing / building table
   *   • If followTerminal was false (user scrolled up during solve):
   *     → sets completedWhilePaused = true
   *     → shows ⊞ button to manually switch to table view later
   *
   * ── ⌄ BUTTON (line ~1830) ────────────────────────────────────────────────
   *   Calls scrollTerminalToBottom() which:
   *     1. Sets followTerminal = true
   *     2. Sets completedWhilePaused = false
   *     3. Scrolls the div to the bottom
   *
   * ── ⊞ BUTTON (line ~1829) ────────────────────────────────────────────────
   *   Appears when completedWhilePaused is true (solve finished while user
   *   was scrolled up). Clicking switches to table view.
   *
   * ── VIEW SWITCHING (terminal ↔ table) ─────────────────────────────────────
   *   switchToTableMode / switchToTerminalMode save the current scroll
   *   position, set isSwitchingViewRef = true, then toggle tableView.
   *   The useLayoutEffect at line ~1223 restores the saved position.
   *   isSwitchingViewRef is cleared by the useEffect at line ~1233 so that
   *   the auto-scroll effect does NOT fire during the switch.
   *
   * ── COMMON PITFALL ────────────────────────────────────────────────────────
   *   The rAF in the auto-scroll effect must NOT call scrollTerminalToBottom()
   *   directly, because that function unconditionally sets followTerminal=true,
   *   which would override a user who just scrolled up. Instead, the rAF
   *   callback must check followTerminalRef.current before scrolling.
   * ============================================================================
   */
  const terminalTextRef = useRef<HTMLDivElement>(null);
  const tableContainerRef = useRef<HTMLDivElement>(null);
  // True only while the user is actively touching/dragging the terminal
  // (mousedown or touch), so fast content growth alone can't be mistaken
  // for a user-initiated scroll-up (see handleTerminalScroll below).
  const terminalUserActiveRef = useRef(false);
  const terminalUserActiveTimeoutRef = useRef<number | undefined>(undefined);
  const markTerminalUserActive = () => {
    terminalUserActiveRef.current = true;
    if (terminalUserActiveTimeoutRef.current !== undefined) {
      window.clearTimeout(terminalUserActiveTimeoutRef.current);
      terminalUserActiveTimeoutRef.current = undefined;
    }
  };
  const scheduleTerminalUserInactive = () => {
    if (terminalUserActiveTimeoutRef.current !== undefined) window.clearTimeout(terminalUserActiveTimeoutRef.current);
    terminalUserActiveTimeoutRef.current = window.setTimeout(() => {
      terminalUserActiveRef.current = false;
      terminalUserActiveTimeoutRef.current = undefined;
    }, 400);
  };
  const terminalScrollPositionRef = useRef(0);
  const tableScrollPositionRef = useRef(0);
  const pageScrollEdgeRef = useRef<"top" | "bottom">("top");
  const pageInputFocused = useRef(false);
  const pageSwitcherRef = useRef<HTMLDivElement>(null);
  const pageInputRef = useRef<HTMLInputElement>(null);
  // Intentional feature by Abid: the page-switcher pill stays opaque as long as
  // autoscroll is running, and flashes opaque on overscroll page changes. After
  // autoscroll stops or the flash is triggered, it returns to its transparent
  // state once the cooldown below has elapsed.
  const PAGE_SWITCHER_COOLDOWN_MS = 2000;
  const [pageSwitcherOpaque, setPageSwitcherOpaque] = useState(false);
  const pageSwitcherHideTimerRef = useRef<number | undefined>(undefined);
  const touchNavRef = useRef<{ startY: number; lastY: number; moved: boolean; node: HTMLDivElement | null } | null>(null);
  const terminalPageRef = useRef(0);
  const tablePageRef = useRef(0);
  const restoreScrollRef = useRef<number | null>(null);
  const firstTableSwitchAfterSolveRef = useRef(true);
  const isSwitchingViewRef = useRef(false);
  const zoomRef = useRef(1);
  const cubeColumnRef = useRef<HTMLDivElement>(null);
  const appRef = useRef<HTMLDivElement>(null);
  const tableMetricRef = useRef("Slice");
  const mainGridRef = useRef<HTMLDivElement>(null);
  const optionsPanelRef = useRef<HTMLDivElement>(null);
  const useLessRamRef = useRef(false);
  const pageRef = useRef(0);
  const pageSizeRef = useRef(1000);
  const showAllRef = useRef(false);
  const ramOffsetRef = useRef(0);
  const offloadedTotalRef = useRef(0);
  const offloadedChunksRef = useRef<Map<number, number>>(new Map());
  const pendingTailRef = useRef<Solution[] | null>(null);
  const offloadBusyRef = useRef(false);
  const offloadPendingRef = useRef(false);

  useEffect(() => {
    const el = document.documentElement;
    const update = () => {
      const bp = readBreakpoints();
      const tall = window.innerHeight / zoomRef.current >= bp.tall;
      el.classList.toggle("tall-viewport", tall);
      if (tall && window.innerWidth / zoomRef.current <= bp.wide) setMobileOutputOpen(false);
    };
    update();
    window.addEventListener("resize", update);
    return () => window.removeEventListener("resize", update);
  }, []);
  useEffect(() => {
    zoomRef.current = zoom;
    document.documentElement.classList.toggle("tall-viewport", window.innerHeight / zoom >= readBreakpoints().tall);
  }, [zoom]);
  useEffect(() => {
    const updateBreakpoints = () => {
      const effW = window.innerWidth / zoomRef.current;
      const bp = readBreakpoints();
      document.documentElement.classList.toggle("bp-720", effW <= bp.wide);
      document.documentElement.classList.toggle("bp-620", effW <= bp.semi);
      document.documentElement.classList.toggle("bp-460", effW <= bp.narrow);
    };
    updateBreakpoints();
    window.addEventListener("resize", updateBreakpoints);
    return () => window.removeEventListener("resize", updateBreakpoints);
  }, []);
  useEffect(() => {
    const update = () => {
      const effW = window.innerWidth / zoomRef.current;
      const bp = readBreakpoints();
      document.documentElement.classList.toggle("bp-720", effW <= bp.wide);
      document.documentElement.classList.toggle("bp-620", effW <= bp.semi);
      document.documentElement.classList.toggle("bp-460", effW <= bp.narrow);
    };
    update();
  }, [zoom]);
  useLayoutEffect(() => {
    const measure = () => {
      const col = cubeColumnRef.current;
      const grid = mainGridRef.current;
      if (col && grid) {
        grid.style.setProperty("--cube-h", col.scrollHeight + "px");
      }
    };
    measure();
    window.addEventListener("resize", measure);
    return () => window.removeEventListener("resize", measure);
  });
  useEffect(() => {
    const measure = () => {
      const col = cubeColumnRef.current;
      const grid = mainGridRef.current;
      if (col && grid) {
        grid.style.setProperty("--cube-h", col.scrollHeight + "px");
      }
    };
    measure();
  }, [zoom, running, undo.length, redo.length, tableView, expanded]);
  useEffect(() => {
    const anyOpen = modal !== null || favoritesOpen;
    if (anyOpen) {
      history.pushState({ modal: true }, "");
    }
    const onPop = () => {
      setModalStack((stack) => {
        if (stack.length > 0) {
          setModal(stack[stack.length - 1]);
          return stack.slice(0, -1);
        }
        setModal(null);
        return stack;
      });
      setShowAllConfirm(false);
      setFavoritesOpen(false);
      setFavoritesClosing(false);
    };
    window.addEventListener("popstate", onPop);
    return () => window.removeEventListener("popstate", onPop);
  }, [modal, favoritesOpen]);
  const navigateModal = (next: Exclude<ModalType, null>) => {
    if (modal) setModalStack((stack) => [...stack, modal]);
    setModal(next);
  };
  useEffect(() => {
    const node = tableContainerRef.current;
    if (!node || !tableView) return;
    const measure = () => setTableWidth(node.clientWidth);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    return () => observer.disconnect();
  }, [tableView]);
  useEffect(() => {
    const panel = optionsPanelRef.current;
    if (!panel) return;
    const update = () => {
      const width = panel.clientWidth;
      const bp = readBreakpoints();
      panel.classList.toggle("panel-narrow", width <= bp.panel);
      panel.classList.toggle("panel-wide", width > bp.panel);
      panel.classList.toggle("panel-tiny", width <= bp.panelTiny);
    };
    update();
    const observer = new ResizeObserver(update);
    observer.observe(panel);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    if (appRef.current) appRef.current.inert = Boolean(modal || diskOpen || weightsOpen || showAllConfirm || favoritesOpen || favoritesClosing);
  }, [modal, diskOpen, weightsOpen, showAllConfirm, favoritesOpen, favoritesClosing]);
  useEffect(() => {
    const suppressButtonFocus = (event: MouseEvent) => {
      if (event.button === 0 && (event.target as Element | null)?.closest("button")) event.preventDefault();
    };
    window.addEventListener("mousedown", suppressButtonFocus);
    return () => window.removeEventListener("mousedown", suppressButtonFocus);
  }, []);
  useEffect(() => {
    const handlePointerDown = (event: MouseEvent) => {
      if (!modeControlRef.current?.contains(event.target as Node)) {
        setModeMenu(false);
      }
      if (!(event.target as Element | null)?.closest(".option-dropdown")) {
        if (document.activeElement instanceof HTMLElement && document.activeElement.closest(".option-dropdown")) {
          document.activeElement.blur();
        }
        setOpenDropdown(null);
      }
      if (!(event.target as Element | null)?.closest(".top-menu-wrap")) {
        setMenu(false);
      }
      if (stickyTooltip && !(event.target as Element | null)?.closest(".sticky-tooltip, .ergo-value")) {
        setStickyTooltip(null);
      }
      if (!contextMenu) return;
      if ((event.target as Element | null)?.closest(".solution-context")) return;
      setContextMenu(null);
    };
    window.addEventListener("pointerdown", handlePointerDown);
    return () => window.removeEventListener("pointerdown", handlePointerDown);
  }, [contextMenu, stickyTooltip]);
  useEffect(() => {
    const onMouseUp = () => scheduleTerminalUserInactive();
    window.addEventListener("mouseup", onMouseUp);
    return () => window.removeEventListener("mouseup", onMouseUp);
  }, []);
  const finalizeSliceHistory = () => {
    const pending = slicePending.current;
    if (!pending.length) return;
    const saved = pending.length % 2 === 0 && pending.length >= 2
      ? [pending[0], pending[pending.length - 1]] : [pending[0]];
    setUndo((old) => [...old, ...saved].slice(-64));
    setRedo([]);
    slicePending.current = [];
    sliceTimer.current = undefined;
  };
  const onCubeChange = (next: CubeState, action = "edit") => {
    const changed = positionString(next) !== positionString(stateRef.current);
    if (!ignoreHistory.current && changed) {
      if (action === "slice") {
        slicePending.current.push(positionString(stateRef.current));
        if (sliceTimer.current !== undefined) window.clearTimeout(sliceTimer.current);
        sliceTimer.current = window.setTimeout(finalizeSliceHistory, 600);
      } else if (action !== "escape") {
        setUndo((old) => [...old, positionString(stateRef.current)].slice(-64));
        setRedo([]);
      }
    }
    stateRef.current = next;
    setCubeState(next);
    setIgnoreMiddle(next.middle === 0);
  };
  useEffect(() => () => {
    if (sliceTimer.current !== undefined) window.clearTimeout(sliceTimer.current);
    if (renderFrame.current !== undefined) cancelAnimationFrame(renderFrame.current);
    if (pageSwitcherHideTimerRef.current !== undefined) window.clearTimeout(pageSwitcherHideTimerRef.current);
  }, []);
  const scrollTerminalToBottom = () => {
    const node = terminalTextRef.current;
    if (!node) return;
    followTerminalRef.current = true;
    setFollowTerminal(true);
    setCompletedWhilePaused(false);
    if (!showAll) {
      const lastPage = Math.max(0, totalPages - 1);
      pageScrollEdgeRef.current = "bottom";
      setPage(lastPage);
    }
    node.scrollTop = node.scrollHeight;
  };
  const openMobileOutput = () => {
    const root = document.documentElement;
    if (!(root.classList.contains("tall-viewport") && root.classList.contains("bp-720"))) {
      setMobileOutputOpen(true);
    }
    requestAnimationFrame(scrollTerminalToBottom);
  };
  // Intentional feature by Abid: temporarily switch the page-switcher pill to
  // opaque so overscroll page changes are noticed, then let it fade back to
  // transparent after the cooldown. While autoscroll is running the pill is
  // already kept opaque, so no flash is needed there.
  const flashPageSwitcher = () => {
    if (followTerminalRef.current) return;
    if (pageSwitcherHideTimerRef.current !== undefined) {
      window.clearTimeout(pageSwitcherHideTimerRef.current);
      pageSwitcherHideTimerRef.current = undefined;
    }
    setPageSwitcherOpaque(true);
    pageSwitcherHideTimerRef.current = window.setTimeout(() => {
      pageSwitcherHideTimerRef.current = undefined;
      setPageSwitcherOpaque(false);
    }, PAGE_SWITCHER_COOLDOWN_MS);
  };
  const goToPage = (next: number, edge: "top" | "bottom") => {
    if (!filterActive && offloadActive() && !isViewInRam(next, totalCountRefs())) setIsRestoring(true);
    setPage(next);
    pageScrollEdgeRef.current = edge;
    if (running && next === totalPages - 1) {
      followTerminalRef.current = true;
      setFollowTerminal(true);
      setCompletedWhilePaused(false);
    } else {
      followTerminalRef.current = false;
      setFollowTerminal(false);
    }
  };
  const commitPageInput = () => {
    const n = parseInt(pageInput, 10);
    if (Number.isFinite(n)) {
      const target = Math.min(totalPages, Math.max(1, n));
      goToPage(target - 1, "top");
    }
    setPageInput(String(clampedPage + 1));
  };
  const handleTerminalWheel = (event: React.WheelEvent<HTMLDivElement>) => {
    const node = terminalTextRef.current;
    if (!node) return;
    if (event.deltaY < 0) {
      if (running) {
        followTerminalRef.current = false;
        setFollowTerminal(false);
      }
      if (node.scrollHeight > node.clientHeight + 4 && node.scrollTop <= 1 && clampedPage > 0) { goToPage(clampedPage - 1, "bottom"); flashPageSwitcher(); }
    } else if (event.deltaY > 0) {
      const atBottom = node.scrollHeight - node.scrollTop - node.clientHeight < 4;
      if (node.scrollHeight > node.clientHeight + 4 && atBottom && clampedPage < totalPages - 1) { goToPage(clampedPage + 1, "top"); flashPageSwitcher(); }
    }
  };
  const handleTableWheel = (event: React.WheelEvent<HTMLDivElement>) => {
    const node = tableContainerRef.current;
    if (!node) return;
    if (event.deltaY < 0 && node.scrollHeight > node.clientHeight + 4 && node.scrollTop <= 1 && clampedPage > 0) { goToPage(clampedPage - 1, "bottom"); flashPageSwitcher(); }
    else if (event.deltaY > 0 && node.scrollHeight > node.clientHeight + 4 && node.scrollHeight - node.scrollTop - node.clientHeight < 4 && clampedPage < totalPages - 1) { goToPage(clampedPage + 1, "top"); flashPageSwitcher(); }
  };
  const handleTouchStart = (event: React.TouchEvent<HTMLDivElement>) => {
    const node = tableView ? tableContainerRef.current : terminalTextRef.current;
    if (!node) return;
    markTerminalUserActive();
    const touch = event.touches[0];
    touchNavRef.current = { startY: touch.clientY, lastY: touch.clientY, moved: false, node };
  };
  const handleTouchMove = (event: React.TouchEvent<HTMLDivElement>) => {
    const state = touchNavRef.current;
    if (!state) return;
    state.lastY = event.touches[0].clientY;
    state.moved = true;
  };
  const handleTouchEnd = () => {
    scheduleTerminalUserInactive();
    const state = touchNavRef.current;
    touchNavRef.current = null;
    const node = state?.node;
    if (!state || !state.moved || !node) return;
    const dy = state.lastY - state.startY;
    if (dy > 30) {
      if (node.scrollHeight > node.clientHeight + 4 && node.scrollTop <= 1 && clampedPage > 0) { goToPage(clampedPage - 1, "bottom"); flashPageSwitcher(); }
    } else if (dy < -30) {
      const atBottom = node.scrollHeight - node.scrollTop - node.clientHeight < 4;
      if (node.scrollHeight > node.clientHeight + 4 && atBottom && clampedPage < totalPages - 1) { goToPage(clampedPage + 1, "top"); flashPageSwitcher(); }
    }
  };
  const handleTerminalScroll = () => {
    const node = terminalTextRef.current;
    if (!node) return;
    const prev = terminalScrollPositionRef.current;
    const next = node.scrollTop;
    terminalScrollPositionRef.current = next;
    if (running && next < prev) {
      const nearBottom = node.scrollHeight - next - node.clientHeight < 50;
      if (!nearBottom && followTerminalRef.current && terminalUserActiveRef.current) {
        followTerminalRef.current = false;
        setFollowTerminal(false);
      }
    }
  };
  const handleTableScroll = () => {
    const node = tableContainerRef.current;
    if (!node) return;
    // Save current table scroll position
    tableScrollPositionRef.current = node.scrollTop;
    // Anchor coords are stale once the row moves under the tooltip.
    if (stickyTooltip) setStickyTooltip(null);
  };
  const switchToTableMode = () => {
    if (terminalTextRef.current) {
      terminalScrollPositionRef.current = terminalTextRef.current.scrollTop;
    }
    if (firstTableSwitchAfterSolveRef.current) {
      terminalPageRef.current = 0;
      terminalScrollPositionRef.current = 0;
      tablePageRef.current = 0;
      tableScrollPositionRef.current = 0;
      firstTableSwitchAfterSolveRef.current = false;
    } else {
      terminalPageRef.current = clampedPage;
    }
    followTerminalRef.current = false;
    setFollowTerminal(false);
    isSwitchingViewRef.current = true;
    restoreScrollRef.current = tableScrollPositionRef.current;
    if (!filterActive && offloadActive() && !isViewInRam(tablePageRef.current, totalCountRefs())) setIsRestoring(true);
    setPage(tablePageRef.current);
    setTableView(true);
  };
  const finishTableBusySoon = () => {
    requestAnimationFrame(() => requestAnimationFrame(() => setTableBusyMessage("")));
  };
  const switchToTerminalMode = () => {
    if (tableContainerRef.current) {
      tableScrollPositionRef.current = tableContainerRef.current.scrollTop;
    }
    tablePageRef.current = clampedPage;
    followTerminalRef.current = false;
    setFollowTerminal(false);
    isSwitchingViewRef.current = true;
    restoreScrollRef.current = terminalScrollPositionRef.current;
    if (!filterActive && offloadActive() && !isViewInRam(terminalPageRef.current, totalCountRefs())) setIsRestoring(true);
    setPage(terminalPageRef.current);
    setTableView(false);
  };
  useEffect(() => {
    if (followTerminal && !isSwitchingViewRef.current && !isRestoring) {
      requestAnimationFrame(() => {
        if (followTerminalRef.current) {
          const node = terminalTextRef.current;
          if (node) node.scrollTop = node.scrollHeight;
        }
      });
    }
  }, [outputLines, statusLines, solutions, tableView, running, followTerminal, isRestoring]);
  useEffect(() => {
    isSwitchingViewRef.current = false;
  }, [tableView]);
  // Intentional feature by Abid: keep the page-switcher pill opaque as long as
  // autoscroll is running; once autoscroll stops (scroll-lock or solve end) it
  // returns to transparent after the cooldown.
  useEffect(() => {
    if (followTerminal) {
      if (pageSwitcherHideTimerRef.current !== undefined) {
        window.clearTimeout(pageSwitcherHideTimerRef.current);
        pageSwitcherHideTimerRef.current = undefined;
      }
      setPageSwitcherOpaque(true);
    } else if (pageSwitcherHideTimerRef.current === undefined) {
      pageSwitcherHideTimerRef.current = window.setTimeout(() => {
        pageSwitcherHideTimerRef.current = undefined;
        setPageSwitcherOpaque(false);
      }, PAGE_SWITCHER_COOLDOWN_MS);
    }
  }, [followTerminal]);
  useEffect(() => {
    pageRef.current = page;
  }, [page]);
  useEffect(() => {
    pageSizeRef.current = pageSize;
  }, [pageSize]);
  useEffect(() => {
    showAllRef.current = showAll;
  }, [showAll]);
  useEffect(() => {
    if (showAll || !followTerminalRef.current) return;
    setPage(Math.max(0, totalPages - 1));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [totalCount, pageSize, showAll]);
  useEffect(() => {
    setPage(0);
  }, [pageSize, showAll]);
  useEffect(() => {
    if (!settingsReady.current) return;
    if (!filterActive && (useLessRam || batchOffload) && !isViewInRam(page, totalCountRefs())) setIsRestoring(true);
    void syncOffload();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [solutions, page, pageSize, useLessRam, batchOffload, showAll, running, filterActive]);
  useEffect(() => {
    const onMouseDown = (event: MouseEvent) => {
      const pill = pageSwitcherRef.current;
      const input = pageInputRef.current;
      if (pill && input && document.activeElement === input && !pill.contains(event.target as Node)) {
        input.blur();
      }
    };
    document.addEventListener("mousedown", onMouseDown);
    return () => document.removeEventListener("mousedown", onMouseDown);
  }, []);
  useLayoutEffect(() => {
    if (followTerminalRef.current || isRestoring) return;
    const node = tableView ? tableContainerRef.current : terminalTextRef.current;
    if (!node) return;
    if (restoreScrollRef.current !== null) {
      node.scrollTop = restoreScrollRef.current;
      restoreScrollRef.current = null;
    } else {
      node.scrollTop = pageScrollEdgeRef.current === "bottom" ? node.scrollHeight : 0;
    }
  }, [page, tableView, isRestoring]);
  useEffect(() => {
    if (!tableBusyMessage) return;
    const id = window.setInterval(() => setTableBusyTick((value) => value + 1), 900);
    return () => window.clearInterval(id);
  }, [tableBusyMessage]);
  useEffect(() => {
    // Gather live rate samples in the background (regardless of whether the debug
    // modal is open) so the graph is always fully populated, including after the
    // solve has finished. History is reset when the next solve starts.
    const id = setInterval(() => {
      const start = solveStartTimeRef.current;
      if (!start) return;
      const now = performance.now();
      const running = runningRef.current;
      const refTime = running ? now : (solveStopTimeRef.current || now);
      const elapsedSec = (refTime - start) / 1000;
      if (elapsedSec <= 0) return;

      const history = rateHistoryRef.current;
      const last = history[history.length - 1];

      if (running) {
        const timestamps = debugStatsRef.current.solutionTimestamps;
        const keepFrom = now - 15000;
        let trim = 0;
        while (trim < timestamps.length && timestamps[trim] <= keepFrom) trim++;
        if (trim) timestamps.splice(0, trim);

        const windowMs = Math.max(1000, Math.min(10000, elapsedSec * 1000));
        const cutoff = refTime - windowMs;
        let count = 0;
        for (let i = timestamps.length - 1; i >= 0; i--) {
          if (timestamps[i] > cutoff) count++;
          else break;
        }
        const solRate = count / (windowMs / 60000);
        const nodeRate = progressRateRef.current;
        if (!last || elapsedSec - last.t >= 0.4) history.push({ t: elapsedSec, sol: solRate, node: nodeRate });
      } else if (!last || elapsedSec - last.t >= 0.05) {
        // Solve finished: push one final point at the stop time, then freeze.
        const windowMs = Math.max(1000, Math.min(10000, elapsedSec * 1000));
        const cutoff = refTime - windowMs;
        const timestamps = debugStatsRef.current.solutionTimestamps;
        let count = 0;
        for (let i = timestamps.length - 1; i >= 0; i--) {
          if (timestamps[i] > cutoff) count++;
          else break;
        }
        const solRate = count / (windowMs / 60000);
        const nodeRate = elapsedSec > 0 ? progressNodesRef.current / elapsedSec : 0;
        history.push({ t: elapsedSec, sol: solRate, node: nodeRate });
      }
      if (history.length > 30000) {
        const compacted: { t: number; sol: number; node: number }[] = [];
        for (let i = 0; i < history.length; i += 2) compacted.push(history[i]);
        rateHistoryRef.current = compacted;
      }
    }, 250);
    return () => clearInterval(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  useEffect(() => {
    // Re-render while the debug modal is open so the grid stats keep updating.
    if (modal !== "debug") return;
    const id = setInterval(() => setDebugTick(t => t + 1), 250);
    return () => clearInterval(id);
  }, [modal]);

  const toggleIgnoreMiddle = (checked: boolean) => {
    const current = stateRef.current;
    if (checked && current.middle !== 0) preIgnoreMiddle.current = current.middle;
    const next = { ...current, middle: checked ? 0 : preIgnoreMiddle.current };
    cubeActions.current?.set(next);
    setIgnoreMiddle(checked);
  };
  const currentRunKey = () => {
    const flags = solverFlags({ metric, all, suboptimal, depths, generator, two, cubeshape: cubeShape, ignoreEquator: ignoreMiddle, angle, maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue });
    if (ignoreTransforms) flags.push("-x");
    if (smartKarn) flags.push("-k2");
    if (outputMode === "karn") flags.push("-k1");
    if (outputMode === "abid") flags.push("-k3");
    return [lastSolvePosition.current || positionString(cubeState), ...flags].join(" ");
  };
  const applyRunConfig = (key: string) => {
    const [position, ...flags] = key.split(/\s+/).filter(Boolean), parsed = parsePosition(position || "");
    if (parsed) restore(position);
    setMetric(flags.includes("-es") ? "es" : flags.includes("-ea") ? "ea" : "move");
    const allFlag = flags.find((flag) => /^-a\d*$/.test(flag));
    setAll(Boolean(allFlag)); setSuboptimal(allFlag && allFlag.length > 2 ? Number(allFlag.slice(2)) : 0);
    const depthFlag = flags.find((flag) => flag.startsWith("-d")); setDepths(depthFlag?.slice(2) || "");
    setGenerator(flags.includes("-g"));
    setTwo(flags.includes("-2") ? "2g" : flags.includes("-p") ? "p2g" : "none");
    setCubeShapeMemory(flags.includes("-c"));
    if (flags.includes("-m") !== ignoreMiddle) { ignoreHistory.current = true; toggleIgnoreMiddle(flags.includes("-m")); ignoreHistory.current = false; }
    setAngle(flags.includes("-nb") ? "nb" : flags.includes("-nu") ? "nu" : flags.includes("-nd") ? "nd" : "none");
    const setLimit = (prefix: string, setEnabled: (v: boolean) => void, setValue: (v: number) => void) => {
      const flag = flags.find((value) => value.startsWith(prefix)); setEnabled(Boolean(flag));
      if (flag) setValue(Number(flag.slice(2)));
    };
    setLimit("-X", setMaxX, setMaxXValue); setLimit("-Y", setMaxY, setMaxYValue); setLimit("-Z", setMaxTotal, setMaxTotalValue);
    setIgnoreTransforms(flags.includes("-x"));
    if (flags.includes("-k1")) setOutputMode("karn");
    else if (flags.includes("-k2")) setOutputMode("cskarn");
    else if (flags.includes("-k3")) setOutputMode("abid");
    solutionsRef.current = [];
    outputLinesRef.current = [];
    seenRaw.current.clear();
    seenDisplay.current.clear();
    setSolutions([]);
    setOutputLines([]);
    setStatusLines([]);
    setRunCubeShape(false);
    setTableView(false);
    history.back();
  };
  const restore = (position: string) => {
    const next = parsePosition(position);
    if (!next) return;
    ignoreHistory.current = true;
    cubeActions.current?.set(next);
    ignoreHistory.current = false;
  };
  const doUndo = () => {
    if (running) return;
    setUndo((stack) => {
      if (!stack.length) return stack;
      const previous = stack[stack.length - 1];
      const snapshot = positionString(stateRef.current);
      restore(previous);
      setRedo((items) => [...items, snapshot].slice(-64));
      return stack.slice(0, -1);
    });
  };
  const doRedo = () => {
    if (running) return;
    setRedo((stack) => {
      if (!stack.length) return stack;
      const next = stack[stack.length - 1];
      const snapshot = positionString(stateRef.current);
      restore(next);
      setUndo((items) => [...items, snapshot].slice(-64));
      return stack.slice(0, -1);
    });
  };
  const chooseMode = (next: string) => {
    setMode(next);
    setInput("");
    setModeMenu(false);
  };
  const cycleMode = () =>
    chooseMode(
      mode === "SCRAMBLE" ? "ALG" : mode === "ALG" ? "POSITION" : "SCRAMBLE",
    );
  const apply = async (fromSolved = false) => {
    let next: CubeState | undefined;
    if (mode === "POSITION") next = parsePosition(input);
    else {
      let raw = input.trim() || "0,0";
      if (mode === "ALG") raw = invertScramble(raw);
      const leadingSlash = /^[\\/]/.test(raw), trailingSlash = raw.length > 1 && /[\\/]$/.test(raw);
      const commaInput = addCommas(raw);
      try {
        raw = tauri()?.core?.invoke ? await tauri()!.core!.invoke<string>("unkarnify", { input: commaInput }) : commaInput;
      } catch { raw = commaInput; }
      if (leadingSlash && !raw.startsWith("/")) raw = "/" + raw;
      if (trailingSlash && !raw.endsWith("/")) raw += "/";
      const base = fromSolved
        ? { position: [...solved], partial: Array(24).fill(0), middle: 1, middlePartial: 0 }
        : cubeState;
      next = applyNumericAlgorithm(base, raw);
    }
    if (!next) {
      setInputError(mode === "POSITION" ? t('errors.invalidPosition') : t('errors.invalidAlg'));
      return;
    }
    cubeActions.current?.set(next);
    setCubeState(next);
    setInputError("");
  };
  const buildDisplayPair = async (line: string, startPosition: string, sliceStart?: string) => {
    const lb = line.lastIndexOf("["), rb = line.lastIndexOf("]");
    if (lb < 0 || rb < 0) return { rawDisplay: line, karnDisplay: line };
    const rawAlg = line.slice(0, lb).trim();
    let karnDisplay = line;
    // Only round-trip through the karnify IPC when karn output is actually
    // shown — in "abid"/"normal" modes the streamed lines already carry every
    // display we need, and any extra invoke per line is pure overhead.
    if (karn) {
      try {
        const converted = await tauri()?.core?.invoke<string>("karnify", {
          input: rawAlg,
          position: smartKarn && !lastSolveCubeShape.current ? startPosition : null,
          generator,
        });
        if (converted) karnDisplay = `${converted}  ${line.slice(lb).trim()}`;
      } catch { /* retain numeric output */ }
    }
    return {
      rawDisplay: injectSliceIndicator(line, sliceStart),
      karnDisplay: injectSliceIndicator(karnDisplay, sliceStart),
    };
  };
  /*
   * LIVE STREAMING — INTENTIONAL FEATURE (by Abid)
   *
   * Solutions appear one-by-one in the terminal as the solver emits them, similar
   * to how AI UIs stream tokens. This is THE magic of the app. It MUST NOT be
   * traded away for raw speed — no batching, no hiding behind a spinner, no
   * waiting for "all results" before showing anything. Every solution must be
   * flushed to the terminal as soon as it is available.
   */
  const addOutputLine = (line: OutputLine) => {
    outputLinesRef.current = [...outputLinesRef.current, line].slice(-1000);
    scheduleSolutionFlush();
  };
  const setRunningState = (value: boolean) => {
    runningRef.current = value;
    setRunning(value);
  };
  const flushSolutionState = () => {
    renderFrame.current = undefined;
    setOutputLines(outputLinesRef.current);
    setSolutions(solutionsRef.current);
  };
  const scheduleSolutionFlush = () => {
    if (renderFrame.current !== undefined) return;
    renderFrame.current = requestAnimationFrame(flushSolutionState);
  };
  const replaceOutputLines = (lines: OutputLine[]) => {
    outputLinesRef.current = lines.slice(-1000);
    scheduleSolutionFlush();
  };
  const addSolution = (row: Solution) => {
    const ramEnd = ramOffsetRef.current + solutionsRef.current.length;
    const pendingLen = pendingTailRef.current?.length ?? 0;
    const total = solutionsRef.current.length + offloadedTotalRef.current + pendingLen;
    if ((useLessRamRef.current || forceOffloadRef.current) && ramEnd < total) {
      const tailStart = total - pendingLen;
      if (pendingLen && ramEnd === tailStart) {
        solutionsRef.current = [...solutionsRef.current, ...pendingTailRef.current!];
        pendingTailRef.current = null;
        setPendingTailCount(0);
      } else {
        pendingTailRef.current = [...(pendingTailRef.current ?? []), row];
        setPendingTailCount(pendingTailRef.current.length);
        scheduleSolutionFlush();
        debugStatsRef.current.solutionTimestamps.push(performance.now());
        return;
      }
    }
    solutionsRef.current = [...solutionsRef.current, row];
    scheduleSolutionFlush();
    debugStatsRef.current.solutionTimestamps.push(performance.now());
  };
  const setSolutionRows = (rows: Solution[]) => {
    solutionsRef.current = rows;
    scheduleSolutionFlush();
  };
  const totalCountRefs = () => offloadedTotalRef.current + solutionsRef.current.length + (pendingTailRef.current?.length ?? 0);
  // Disk offloading of solution pages kicks in when the user enables
  // "useLessRam", or is forced on automatically while batch results exist so a
  // million-row batch never has to keep every solution in memory.
  const offloadActive = () => useLessRamRef.current || forceOffloadRef.current;
  const offloadRange = async (start: number, rows: Solution[]) => {
    if (!rows.length) return;
    for (let i = 0; i < rows.length; i += OFFLOAD_CHUNK) {
      const chunk = rows.slice(i, i + OFFLOAD_CHUNK);
      const chunkStart = start + i;
      offloadedChunksRef.current.set(chunkStart, chunk.length);
      await writeOffloadedChunk(chunkStart, chunk);
    }
    offloadedTotalRef.current += rows.length;
  };
  const restorePage = async (viewStart: number, viewEnd: number, tailStart: number, total: number) => {
    const loaded: { index: number; row: Solution }[] = [];
    const diskEnd = Math.min(viewEnd, tailStart);
    if (viewStart < diskEnd) {
      const chunks = [...offloadedChunksRef.current.entries()].sort((a, b) => a[0] - b[0]);
      for (const [chunkStart, count] of chunks) {
        const chunkEnd = chunkStart + count;
        if (chunkEnd <= viewStart || chunkStart >= diskEnd) continue;
        const raw = await readOffloadedChunk(chunkStart);
        if (!raw || raw.length !== count) {
          offloadedChunksRef.current.delete(chunkStart);
          offloadedTotalRef.current -= count;
          continue;
        }
        const from = Math.max(0, viewStart - chunkStart);
        const to = Math.min(count, diskEnd - chunkStart);
        for (let i = from; i < to; i++) loaded.push({ index: chunkStart + i, row: raw[i] });
        offloadedChunksRef.current.delete(chunkStart);
        offloadedTotalRef.current -= count;
        await removeOffloadedChunk(chunkStart);
        const before = raw.slice(0, from);
        if (before.length) { offloadedChunksRef.current.set(chunkStart, before.length); offloadedTotalRef.current += before.length; await writeOffloadedChunk(chunkStart, before); }
        const after = raw.slice(to);
        if (after.length) { const aStart = chunkStart + to; offloadedChunksRef.current.set(aStart, after.length); offloadedTotalRef.current += after.length; await writeOffloadedChunk(aStart, after); }
      }
      loaded.sort((a, b) => a.index - b.index);
    }
    let result = loaded.map((x) => x.row);
    if (viewEnd > tailStart) {
      const from = Math.max(0, viewStart - tailStart);
      if (from > 0) await offloadRange(tailStart, (pendingTailRef.current ?? []).slice(0, from));
      const pending = pendingTailRef.current ?? [];
      const to = Math.min(pending.length, viewEnd - tailStart);
      result = [...result, ...pending.slice(from, to)];
      if (to >= pending.length) pendingTailRef.current = null;
      else pendingTailRef.current = pending.slice(to);
      setPendingTailCount(pendingTailRef.current?.length ?? 0);
    }
    return result;
  };
  const flushPendingTailToDisk = async () => {
    const pending = pendingTailRef.current;
    if (!pending?.length) { pendingTailRef.current = null; setPendingTailCount(0); return; }
    const total = totalCountRefs();
    await offloadRange(total - pending.length, pending);
    pendingTailRef.current = null;
    setPendingTailCount(0);
  };
  const restoreAll = async () => {
    const chunks = [...offloadedChunksRef.current.entries()].sort((a, b) => a[0] - b[0]);
    const ramOffset = ramOffsetRef.current;
    const full: Solution[] = [];
    let inserted = false;
    for (const [start, count] of chunks) {
      if (!inserted && start >= ramOffset) { full.push(...solutionsRef.current); inserted = true; }
      const raw = await readOffloadedChunk(start);
      if (raw && raw.length === count) full.push(...raw);
      await removeOffloadedChunk(start);
    }
    if (!inserted) full.push(...solutionsRef.current);
    const pending = pendingTailRef.current ?? [];
    pendingTailRef.current = null;
    setPendingTailCount(0);
    full.push(...pending);
    offloadedChunksRef.current.clear();
    offloadedTotalRef.current = 0;
    ramOffsetRef.current = 0;
    solutionsRef.current = full;
    setSolutions([...full]);
    setOffloadedTotal(0);
  };
  const isViewInRam = (view: number, total: number) => {
    const pc = computeTotalPages(total, pageSizeRef.current);
    const v = Math.min(Math.max(view, 0), pc - 1);
    const vs = v * pageSizeRef.current;
    const ve = v === pc - 1 ? total : Math.min(vs + pageSizeRef.current, total);
    const rs = ramOffsetRef.current;
    const re = rs + solutionsRef.current.length;
    return vs >= rs && ve <= re;
  };
  const clearSolutions = () => {
    solutionsRef.current = [];
    pendingTailRef.current = null;
    setPendingTailCount(0);
    offloadedChunksRef.current.clear();
    offloadedTotalRef.current = 0;
    ramOffsetRef.current = 0;
    setOffloadedTotal(0);
    setSolutions([]);
    forceOffloadRef.current = false;
    setBatchOffload(false);
    batchSolvedRef.current = 0;
    setBatchSolved(0);
    setBatchStatsText(null);
    void clearOffloadedSolutions();
  };
  const syncOffload = async () => {
    if (!offloadActive()) {
      if (offloadedTotalRef.current || (pendingTailRef.current?.length ?? 0)) {
        if (offloadBusyRef.current) { offloadPendingRef.current = true; return; }
        offloadBusyRef.current = true;
        try { await restoreAll(); } finally {
          offloadBusyRef.current = false;
          if (offloadPendingRef.current) { offloadPendingRef.current = false; void syncOffload(); }
        }
      }
      return;
    }
    if (offloadBusyRef.current) { offloadPendingRef.current = true; return; }
    offloadBusyRef.current = true;
    try {
      let total = totalCountRefs();
      if (!total) { setIsRestoring(false); return; }
      if (!runningRef.current && (pendingTailRef.current?.length ?? 0)) {
        await flushPendingTailToDisk();
        total = totalCountRefs();
      }
      const pageCount = computeTotalPages(total, pageSizeRef.current);
      const view = Math.min(Math.max(pageRef.current, 0), pageCount - 1);
      const viewStart = view * pageSizeRef.current;
      const viewEnd = view === pageCount - 1 ? total : Math.min(viewStart + pageSizeRef.current, total);
      const ramOffset = ramOffsetRef.current;
      const ramEnd = ramOffset + solutionsRef.current.length;
      if (showAllRef.current) {
        if (solutionsRef.current.length === total) { setIsRestoring(false); return; }
        setIsRestoring(true);
        await offloadRange(ramOffset, solutionsRef.current);
        solutionsRef.current = [];
        ramOffsetRef.current = 0;
        await restoreAll();
        setIsRestoring(false);
        return;
      }
      if (viewStart >= ramOffset && viewEnd <= ramEnd) {
        if (runningRef.current) {
          if (viewStart > ramOffset) {
            await offloadRange(ramOffset, solutionsRef.current.slice(0, viewStart - ramOffset));
            const cur = solutionsRef.current;
            solutionsRef.current = cur.slice(viewStart - ramOffset);
            ramOffsetRef.current = viewStart;
            setSolutions([...solutionsRef.current]);
            setOffloadedTotal(offloadedTotalRef.current);
          }
          setIsRestoring(false);
          return;
        }
        if (viewStart > ramOffset) {
          await offloadRange(ramOffset, solutionsRef.current.slice(0, viewStart - ramOffset));
        }
        const cur = solutionsRef.current;
        const keep = viewEnd - viewStart;
        if (cur.length > viewEnd - ramOffset) {
          await offloadRange(viewEnd, cur.slice(viewEnd - ramOffset));
        }
        const cur2 = solutionsRef.current;
        solutionsRef.current = cur2.slice(viewStart - ramOffset, viewStart - ramOffset + keep);
        ramOffsetRef.current = viewStart;
        setSolutions([...solutionsRef.current]);
        setOffloadedTotal(offloadedTotalRef.current);
        setIsRestoring(false);
        return;
      }
      setIsRestoring(true);
      await offloadRange(ramOffset, solutionsRef.current);
      solutionsRef.current = [];
      ramOffsetRef.current = 0;
      const freshTotal = totalCountRefs();
      const freshPageCount = computeTotalPages(freshTotal, pageSizeRef.current);
      const freshView = Math.min(Math.max(pageRef.current, 0), freshPageCount - 1);
      const freshStart = freshView * pageSizeRef.current;
      const freshEnd = freshView === freshPageCount - 1 ? freshTotal : Math.min(freshStart + pageSizeRef.current, freshTotal);
      const pendingLen = pendingTailRef.current?.length ?? 0;
      const loaded = await restorePage(freshStart, freshEnd, freshTotal - pendingLen, freshTotal);
      solutionsRef.current = loaded;
      ramOffsetRef.current = freshStart;
      setSolutions([...loaded]);
      setOffloadedTotal(offloadedTotalRef.current);
      setIsRestoring(false);
    } finally {
      offloadBusyRef.current = false;
      if (offloadPendingRef.current) { offloadPendingRef.current = false; void syncOffload(); }
    }
  };
  // Renders a solution line in the current outputMode. Shared by the live
  // stream (dedup key), the solution rows, the terminal and the table so the
  // same algorithm always renders identically everywhere.
  const buildDisplayText = (rawDisplay: string, karnDisplay: string, abidDisplay: string | undefined, sliceStart: string | undefined) => {
    const base = normalizeLine(karn ? karnDisplay : rawDisplay, normalize);
    if (outputMode === "abid" && abidDisplay) return normalizeLine(abidDisplay, normalize);
    return notationStyle(base, outputMode, !!sliceStart);
  };
  const receiveSolverLine = async (line: string, startPosition: string, runId: number) => {
    if (stopped.current) return;
    if (runId !== solveRunId.current) return;
    if (line.startsWith("__PROGRESS__")) {
      if (!solveStartTimeRef.current) solveStartTimeRef.current = performance.now();
      const nm = line.match(/nodes=(\d+)/);
      if (nm) {
        const nodes = parseInt(nm[1], 10);
        const now = performance.now();
        if (lastProgressNodesRef.current > 0) {
          const dt = (now - lastProgressAtRef.current) / 1000;
          if (dt > 0) progressRateRef.current = (nodes - lastProgressNodesRef.current) / dt;
        }
        lastProgressNodesRef.current = nodes;
        lastProgressAtRef.current = now;
        progressNodesRef.current = nodes;
      }
      return;
    }
    const lb = line.lastIndexOf("["), rb = line.lastIndexOf("]");
    if (lb < 0 || rb < 0) {
      if (debugOutputRef.current || !seenRaw.current.size) addOutputLine({ raw: line, karn: line, isSolution: false });
      return;
    }
    const rawAlg = line.slice(0, lb).trim();
    if (seenRaw.current.has(rawAlg)) return;
    seenRaw.current.add(rawAlg);
    const metricsPart = line.slice(lb, rb + 1);
    const afterMetrics = line.slice(rb + 1).trim();
    let rating: RatingResult | undefined, sliceStart: string | undefined;
    let rawDisplay: string, karnDisplay: string, abidDisplay: string | undefined;
    if (afterMetrics) {
      // Extended line layout: <karn>  R{...rating...}  <abid>. Every field is
      // separated by two spaces, and the karn/abid texts themselves only ever
      // contain single spaces, so a plain split on two spaces recovers the
      // fields. The abid field is emitted only when the solve ran with -k3;
      // the rating block only appears for cubeshape solves. Either may be
      // absent, but the karn field is always present in extended output.
      const fields = afterMetrics.split("  ");
      const karnified = fields[0].trim();
      let abidified: string | undefined;
      if (fields[1] && fields[1].startsWith("R{")) {
        try {
          const raw = JSON.parse(fields[1].slice(1));
          rating = { finalScore: raw.f, sliceStart: raw.ss, phase1: raw.p1, phase2: raw.p2, phase3: raw.p3, phase4: raw.p4, weight1: raw.w1, weight2: raw.w2, weight3: raw.w3, weight4: raw.w4, ergoUp: raw.eu, ergoDown: raw.ed, sliceCount: raw.sc, movement: raw.mv, bonus: raw.bn, valid: true };
          if (rating.valid) sliceStart = ratingSliceStart(rating);
        } catch { /* unrated */ }
        abidified = fields.slice(2).join("  ") || undefined;
      } else {
        abidified = fields.slice(1).join("  ") || undefined;
      }
      rawDisplay = injectSliceIndicator(rawAlg + "  " + metricsPart, sliceStart);
      karnDisplay = injectSliceIndicator(karnified + "  " + metricsPart, sliceStart);
      if (abidified) abidDisplay = abidified + "  " + metricsPart;
    } else {
      // Legacy format (no extended data): use IPC fallback
      if (lastSolveCubeShape.current && tauri()?.core?.invoke) {
        try {
          rating = await tauri()!.core!.invoke<RatingResult>("rate_algorithm", { algorithm: rawAlg, initialTopA: /^[1-8XYZ]/i.test(startPosition) });
          if (runId !== solveRunId.current) return;
          if (rating.valid) sliceStart = ratingSliceStart(rating);
        } catch {}
      }
      const pair = await buildDisplayPair(line, startPosition, sliceStart);
      if (runId !== solveRunId.current) return;
      rawDisplay = pair.rawDisplay;
      karnDisplay = pair.karnDisplay;
    }
    let debugAnn: string | undefined;
    if (debugOutputRef.current) debugAnn = ratingDebugAnnotation(rating);
    const displayAlg = lineAlg(buildDisplayText(rawDisplay, karnDisplay, abidDisplay, sliceStart));
    if (seenDisplay.current.has(displayAlg)) return;
    seenDisplay.current.add(displayAlg);
    if (seenRaw.current.size === 1) {
      firstSolutionAt.current = performance.now();
      if (!solveStartTimeRef.current) solveStartTimeRef.current = firstSolutionAt.current;
      if (!debugOutputRef.current) replaceOutputLines(outputLinesRef.current.filter((entry) => entry.isSolution));
    }
    const counts = parseSolutionCounts(line);
    const cleanLine = rawAlg + "  " + metricsPart;
    const row: Solution = { raw: cleanLine, rawDisplay, karnDisplay, abidDisplay, algRaw: rawAlg, ...counts, ergoRaw: rating?.valid ? ratingScore(rating) : undefined, sliceStart, debugAnn };
    addSolution(row);
    addOutputLine({ raw: rawDisplay, karn: karnDisplay, isSolution: true, algRaw: rawAlg, sliceStart });
  };
  const solve = async () => {
    const native = tauri();
    if (!native?.core?.invoke) {
      const nativeMsg = t('status.nativeUnavailable');
      const fallback = { raw: nativeMsg, karn: nativeMsg, isSolution: false };
      outputLinesRef.current = [fallback];
      setOutputLines([fallback]);
      openMobileOutput();
      return;
    }
    if (runningRef.current) {
      stopped.current = true;
      const stopMsg = t('status.stopRequested');
      addOutputLine({ raw: stopMsg, karn: stopMsg, isSolution: false });
      setStatusLines((lines) => [...lines, t('status.stopReady')].slice(-8));
      setRunningState(false);
      void native.core.invoke("stop_solver").catch(() => undefined);
      return;
    }
    if ((two === "2g" && (twoGenStatus.compatibility < 2 || (cubeShape && !twoGenStatus.cornersTwo))) ||
        (two === "p2g" && (twoGenStatus.compatibility < 1 || (cubeShape && !twoGenStatus.cornersPseudo)))) return;
    openMobileOutput();
    const flags = solverFlags({ metric, all, suboptimal, depths, generator, two, cubeshape: cubeShape, ignoreEquator: ignoreMiddle, angle, maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue });
    if (ignoreTransforms) flags.push("-x");
    if (smartKarn) flags.push("-k2");
    if (outputMode === "karn") flags.push("-k1");
    if (outputMode === "abid") flags.push("-k3");
    if (debugOutput) flags.push("-v5");
    stopped.current = false;
    solutionsRef.current = [];
    outputLinesRef.current = [];
    ramOffsetRef.current = 0;
    offloadedTotalRef.current = 0;
    offloadedChunksRef.current.clear();
    pendingTailRef.current = null;
    setOffloadedTotal(0);
    setPendingTailCount(0);
    setIsRestoring(false);
    filterSearchIdRef.current++;
    setFilterOpen(false);
    setFilterQuery("");
    setFilterResults(null);
    setFilterAppliedQuery("");
    setFilterSearching(false);
    setFilterInvalid(false);
    forceOffloadRef.current = false;
    setBatchOffload(false);
    batchSolvedRef.current = 0;
    setBatchSolved(0);
    setBatchStatsText(null);
    if (useLessRamRef.current) void clearOffloadedSolutions();
    seenRaw.current.clear();
    seenDisplay.current.clear();
    lineQueue.current = Promise.resolve();
    firstSolutionAt.current = 0;
    solveStartTimeRef.current = 0;
    solveStopTimeRef.current = 0;
    debugStatsRef.current = { solutionTimestamps: [] };
    rateHistoryRef.current = [];
    progressNodesRef.current = 0;
    progressRateRef.current = 0;
    lastProgressNodesRef.current = 0;
    lastProgressAtRef.current = 0;
    followTerminalRef.current = true;
    lastSolveCubeShape.current = cubeShape;
    firstTableSwitchAfterSolveRef.current = true;
    tableMetricRef.current = metric;
    terminalScrollPositionRef.current = 0;
    tableScrollPositionRef.current = 0;
    terminalPageRef.current = 0;
    tablePageRef.current = 0;
    restoreScrollRef.current = null;
    setRunCubeShape(cubeShape);
    const solvingMsg = t('status.solving');
    setOutputLines([{ raw: solvingMsg, karn: solvingMsg, isSolution: false }]);
    outputLinesRef.current = [{ raw: solvingMsg, karn: solvingMsg, isSolution: false }];
    setStatusLines([]);
    setSolutions([]);
    setPage(0);
    setFollowTerminal(true);
    setTableView(false);
    setTableBusyMessage("");
    setCompletedWhilePaused(false);
    setRunningState(true);
    const runId = ++solveRunId.current;
    const start = positionString(cubeState), startedAt = performance.now();
    lastSolvePosition.current = start;
    try {
      if (!native.Channel) throw new Error(t('status.channelUnavailable'));
      const onLine = new native.Channel<string>();
      onLine.onmessage = async (line) => {
        if (runId !== solveRunId.current) return;
        lineQueue.current = lineQueue.current.then(() => receiveSolverLine(line, start, runId));
      };
      const result = await native.core.invoke<{ code: number | null; stdout: string; stderr: string }>("solve", { position: start, flags, onLine });
      if (runId !== solveRunId.current) return;
      const shouldAutoTable = followTerminalRef.current;
      // Intentional feature by Abid: only auto-switch to table view when there are
      // 2+ solutions, since table view is only useful for comparing/organizing output.
      if (shouldAutoTable && totalCountRefs() >= 2) {
        switchToTableMode();
        setTableBusyMessage(t('table.busyResolving'));
      }
      await lineQueue.current;
      if (shouldAutoTable) setTableBusyMessage(t('table.busyRating'));
      for (const line of `${result.stderr || ""}`.split(/\r?\n/).filter(Boolean))
        await receiveSolverLine(line, start, runId);
      if (runId !== solveRunId.current) return;
      if (lastSolveCubeShape.current && !useLessRamRef.current) {
        if (shouldAutoTable && totalCountRefs()) setTableBusyMessage(t('table.busyNormalizing'));
        setSolutionRows(medianNormalize(solutionsRef.current));
      }
      if (shouldAutoTable) setTableBusyMessage(t('table.busyBuilding'));
      flushSolutionState();
      const count = totalCountRefs();
      const stateLabel = stopped.current ? t('status.stopped') : result.code === 0 ? t('status.done') : t('status.error');
      const status = t('status.summary')
        .replace('{{state}}', stateLabel)
        .replace('{{count}}', String(count))
        .replace('{{plural}}', count === 1 ? "" : "s")
        .replace('{{time}}', ((performance.now() - startedAt) / 1000).toFixed(2));
      setStatusLines(!useLessRamRef.current && lastSolveCubeShape.current && count ? [t('status.ranked').replace('{{count}}', String(count))] : [status]);
      // Intentional feature by Abid: only auto-switch to table view when 2+ solutions
      // exist, since table view is only useful for comparing/organizing output.
      if (shouldAutoTable && count >= 2) {
        switchToTableMode();
        finishTableBusySoon();
      } else if (count) {
        setCompletedWhilePaused(true);
        setTableBusyMessage("");
      } else {
        setTableBusyMessage("");
      }
    } catch (error) {
      setTableBusyMessage("");
      setStatusLines((lines) => [...lines, t('status.errorPrefix') + String(error)].slice(-8));
    } finally {
      if (runId === solveRunId.current) { solveStopTimeRef.current = performance.now(); setRunningState(false); }
    }
  };
  const batchLineCount = (text: string) => {
    let count = 0;
    let start = 0;
    while (start <= text.length) {
      const end = text.indexOf("\n", start);
      const rawLine = end === -1 ? text.slice(start) : text.slice(start, end);
      if (rawLine.trim().length > 0) count++;
      if (end === -1) break;
      start = end + 1;
    }
    return count;
  };
  const batchNonEmptyLines = (text: string) => text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.length > 0);
  // Stores the given (trimmed, non-empty) lines as BATCH_INPUT_CHUNK_SIZE-line
  // chunks in storage.  The whole batch input never lives in memory as a
  // single string once it is big enough to be spooled.
  const storeBatchLines = async (lines: string[]) => {
    if (!lines.length) return;
    for (let i = 0; i < lines.length; i += BATCH_INPUT_CHUNK_SIZE) {
      await writeBatchInputChunk(batchInputChunkCountRef.current++, lines.slice(i, i + BATCH_INPUT_CHUNK_SIZE));
    }
  };
  const appendBatchLines = async (text: string) => {
    const newLines = batchNonEmptyLines(text);
    if (!newLines.length) return;
    if (batchStoredRef.current) {
      await storeBatchLines(newLines);
      setBatchStoredLinesState(batchStoredLinesRef.current + newLines.length);
      return;
    }
    const existing = batchLineCount(batchInput);
    if (existing + newLines.length <= BATCH_INLINE_MAX_LINES) {
      setBatchInput((prev) => (prev ? prev + "\n" : "") + text);
    } else {
      // Too big for the editable textarea: spool everything to storage and
      // show a "Pasted N lines" summary instead of rendering the text.
      const all = [...batchNonEmptyLines(batchInput), ...newLines];
      setBatchInput("");
      setBatchStored(true);
      await storeBatchLines(all);
      setBatchStoredLinesState(all.length);
    }
  };
  const clearBatchInput = async () => {
    setBatchInput("");
    setBatchStored(false);
    setBatchStoredLinesState(0);
    batchInputChunkCountRef.current = 0;
    await clearBatchInputChunks();
  };
  // Async iteration over every solution row that exists right now (offloaded
  // chunks from disk, the in-RAM page window and the pending tail).  Used to
  // rebuild batch CSV/stats without ever holding all results in memory.
  const forEachSolution = async (visit: (row: Solution) => void) => {
    const chunks = [...offloadedChunksRef.current.entries()].sort((a, b) => a[0] - b[0]);
    for (const [start] of chunks) {
      const raw = await readOffloadedChunk(start);
      if (raw) for (const row of raw) visit(row);
    }
    for (const row of solutionsRef.current) visit(row);
    const pending = pendingTailRef.current ?? [];
    for (const row of pending) visit(row);
  };
  const batchSolve = async () => {
    const native = tauri();
    if (!native?.core?.invoke) return;
    const invoke = native.core.invoke;
    const Channel = native.Channel;
    const stored = batchStoredRef.current;
    const totalLines = stored ? batchStoredLinesRef.current : batchLineCount(batchInput);
    if (!totalLines) return;
    const targets = batchTarget.trim() ? batchNonEmptyLines(batchTarget) : [];
    batchStopRef.current = false;
    forceOffloadRef.current = true;
    setBatchOffload(true);
    batchSolvedRef.current = 0;
    setBatchSolved(0);
    batchIndexRef.current = 0;
    setBatchStatsText(null);
    batchRunningRef.current = true;
    setBatchRunning(true);
    solutionsRef.current = [];
    outputLinesRef.current = [];
    ramOffsetRef.current = 0;
    offloadedTotalRef.current = 0;
    offloadedChunksRef.current.clear();
    pendingTailRef.current = null;
    setOffloadedTotal(0);
    setPendingTailCount(0);
    setIsRestoring(false);
    void clearOffloadedSolutions();
    seenRaw.current.clear();
    seenDisplay.current.clear();
    lineQueue.current = Promise.resolve();
    followTerminalRef.current = true;
    setFollowTerminal(true);
    setTableView(false);
    setPage(0);
    const headerMsg = targets.length ? `Batch: ${totalLines} positions -> ${targets.length} target${targets.length !== 1 ? "s" : ""}` : `Batch: ${totalLines} positions`;
    setOutputLines([{ raw: headerMsg, karn: headerMsg, isSolution: false }]);
    outputLinesRef.current = [{ raw: headerMsg, karn: headerMsg, isSolution: false }];
    setStatusLines([]);
    setSolutions([]);
    setRunningState(true);
    const runId = ++solveRunId.current;
    const startedAt = performance.now();
    const flags = solverFlags({ metric, all: false, suboptimal: 0, depths: "", generator: false, two, cubeshape: cubeShape, ignoreEquator: ignoreMiddle, angle, maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue });
    if (ignoreTransforms) flags.push("-x");
    if (debugOutput) flags.push("-v2");
    if (!Channel) return;
    let captureSolution: { rawAlg: string; slices: number; moves: number; angle: number } | null = null;
    const onLine = new Channel<string>();
    onLine.onmessage = (line: string) => {
      if (runId !== solveRunId.current) return;
      const lb = line.lastIndexOf("["), rb = line.lastIndexOf("]");
      if (lb < 0 || rb < 0) return;
      if (captureSolution) return;
      const rawAlg = line.slice(0, lb).trim();
      const counts = parseSolutionCounts(line);
      captureSolution = { rawAlg, slices: counts.slices, moves: counts.moves, angle: counts.angle };
    };
    const flushCapture = async () => {
      await batchQueueRef.current;
      const sol = captureSolution;
      captureSolution = null;
      return sol;
    };
    const flushBatchSolved = () => {
      if (batchProgressFrameRef.current !== undefined) return;
      batchProgressFrameRef.current = requestAnimationFrame(() => {
        batchProgressFrameRef.current = undefined;
        setBatchSolved(batchSolvedRef.current);
      });
    };
    // Batch results stream straight into the normal solutions pipeline so they
    // are paginated and page-offloaded to disk exactly like single-solve output.
    const storeBatchResult = (input: string, sol: { rawAlg: string; slices: number; moves: number; angle: number }, frequency = 1) => {
      const countVal = sol[batchMetricKey] || sol.slices;
      const idx = batchIndexRef.current++;
      const output = `[${idx}] ${input} -> ${sol.rawAlg} [${countVal} ${batchMetricLabel}]${frequency > 1 ? ` (x${frequency})` : ""}`;
      const row: Solution = {
        raw: output, rawDisplay: output, karnDisplay: output, algRaw: output,
        slices: sol.slices, moves: sol.moves, angle: sol.angle,
        input, solution: sol.rawAlg, frequency, rawPassThrough: true,
      };
      addSolution(row);
      batchSolvedRef.current += frequency;
      flushBatchSolved();
    };
    // Rotation-class grouping (slice metric only): inputs that differ only by
    // free U/D turns solve in the same number of slices, so we can solve one
    // representative per group and weight the stats by group size.  Only
    // applies in slice metric where top/bottom turns are free.
    const enableGrouping = metric === "es";
    // Solves a list of rotation-class groups (each group = one representative to
    // solve + the list of inputs it represents).  Shared by per-chunk grouping
    // and the global bucket-grouping used for huge stored inputs.
    const dbg = (line: string) => {
      if (!debugOutput) return;
      addOutputLine({ raw: line, karn: line, isSolution: false });
      void invoke("debug_log", { line }).catch(() => undefined);
    };
    // Runs one IDA* search for a single input against its targets (or the solved
    // state) on this run's channel and captures the first solution found.
    const solveToSolution = async (given: string): Promise<{ rawAlg: string; slices: number; moves: number; angle: number } | null> => {
      if (targets.length) {
        const candidates: string[] = [];
        const seen = new Set<string>();
        for (let ti = 0; ti < targets.length; ti++) {
          const t = targets[ti];
          const perTarget = generateRemapCandidates(given, t, false);
          dbg(`  Target ${ti + 1}: "${t}" -> ${perTarget.length ? perTarget.join(" | ") : "(none)"}`);
          for (const c of perTarget) {
            if (!seen.has(c)) { seen.add(c); candidates.push(c); }
          }
        }
        dbg(`  All candidates (${candidates.length}): ${candidates.join(" | ")}`);
        captureSolution = null;
        try {
          await invoke("batch_solve_multi", { candidates, onLine });
        } catch { /* skip */ }
        return flushCapture();
      }
      captureSolution = null;
      try {
        await invoke("batch_solve_position", { position: given, onLine });
      } catch { /* skip */ }
      return flushCapture();
    };
    // Secondary, target-frame grouping: distinct rotation classes of the INPUT
    // can translate onto rotation-equivalent positions inside a target's frame
    // (a free U/D turn in slice metric just re-rotates the translated state).
    // Sorted per-target rotation-class keys fully describe the candidate set, so
    // classes sharing a key solve identically and one IDA* search answers them
    // all.  Only meaningful when solving toward explicit targets.
    const targetFrameKey = (given: string): string =>
      targets.map((t) =>
        generateRemapCandidates(given, t, false)
          .map((c) => computeRotationClassKey(c, cubeShape))
          .sort()
          .join(",")
      ).join("|");
    const bucketKeyOf = (l: string): string =>
      (enableGrouping && targets.length) ? targetFrameKey(l) : computeRotationClassKey(l, cubeShape);
    const solveGroupList = async (groups: { reps: string[]; key: string }[]) => {
      if (enableGrouping && targets.length && groups.length > 1) {
        let mergedGroups = 0;
        const byFrame = new Map<string, { reps: string[]; key: string }[]>();
        for (const g of groups) {
          const k = targetFrameKey(g.reps[0]);
          const arr = byFrame.get(k);
          if (arr) arr.push(g); else byFrame.set(k, [g]);
        }
        for (const merged of byFrame.values()) {
          if (batchStopRef.current) return;
          if (merged.length > 1) mergedGroups++;
          const given = merged[0].reps[0];
          dbg(`Given: "${given}" (${merged.length} class${merged.length !== 1 ? "es" : ""} merged, x${merged.reduce((sum, g) => sum + g.reps.length, 0)})`);
          const sol = await solveToSolution(given);
          if (sol) {
            dbg(`  Solved: "${given}" -> ${sol.rawAlg} (${sol[batchMetricKey] || sol.slices} ${batchMetricLabel})`);
            for (const g of merged) storeBatchResult(g.reps[0], sol, g.reps.length);
          }
        }
        if (mergedGroups) {
          const progress = `Secondary grouping: ${groups.length} rotation classes -> ${byFrame.size} unique target-frame classes (reused for ${mergedGroups})`;
          addOutputLine({ raw: progress, karn: progress, isSolution: false });
        }
        return;
      }
      for (const g of groups) {
        if (batchStopRef.current) return;
        const given = g.reps[0];
        dbg(`Given: "${given}"${g.reps.length > 1 ? ` (group x${g.reps.length})` : ""}`);
        const sol = await solveToSolution(given);
        if (sol) {
          dbg(`  Solved: "${given}" -> ${sol.rawAlg} (${sol[batchMetricKey] || sol.slices} ${batchMetricLabel})`);
          storeBatchResult(given, sol, g.reps.length);
        }
      }
    };
    // Groups a single batch of lines into rotation classes (per-chunk grouping;
    // for a whole stored input the bucket path below is preferred so classes
    // are merged across chunk boundaries).
    const processChunk = async (chunkLines: string[]) => {
      let groups: { reps: string[]; key: string }[] = chunkLines.map((l) => ({ reps: [l], key: "" }));
      if (enableGrouping) {
        const byKey = new Map<string, string[]>();
        for (const l of chunkLines) {
          const key = computeRotationClassKey(l, cubeShape);
          const arr = byKey.get(key);
          if (arr) arr.push(l); else byKey.set(key, [l]);
        }
        groups = [...byKey.entries()].map(([key, reps]) => ({ reps, key }));
      }
      await solveGroupList(groups);
    };
    // Global rotation-class grouping for huge stored inputs: partition classes
    // into hash buckets on disk, then load/group one bucket (~chunk sized) into
    // memory at a time, so grouping a million inputs never needs the whole set
    // in RAM and classes are still merged across chunk boundaries.
    const groupedBucketSolve = async (chunkCount: number) => {
      const n = Math.max(1, Math.ceil(totalLines / BATCH_GROUP_BUCKET_LINES));
      addOutputLine({ raw: `Grouping ${totalLines} positions into rotation classes`, karn: `Grouping ${totalLines} positions into rotation classes`, isSolution: false });
      for (let b = 0; b < n; b++) {
        if (batchStopRef.current) return;
        const byKey = new Map<string, string[]>();
        let bucketLines = 0;
        for (let ci = 0; ci < chunkCount; ci++) {
          if (batchStopRef.current) return;
          const chunk = (await readBatchInputChunk(ci)) ?? [];
          for (const l of chunk) {
            // Bucket by the class that fully determines the candidate set, so
            // the whole mergeable group lands in one bucket.  Primary grouping
            // inside the bucket still happens on the plain rotation class.
            if (fnv1a(bucketKeyOf(l)) % n !== b) continue;
            bucketLines++;
            const key = computeRotationClassKey(l, cubeShape);
            const arr = byKey.get(key);
            if (arr) arr.push(l); else byKey.set(key, [l]);
          }
        }
        if (batchStopRef.current) return;
        const groups = [...byKey.entries()].map(([key, reps]) => ({ reps, key }));
        if (groups.length) {
          addOutputLine({ raw: `Grouping bucket ${b + 1}/${n}: ${bucketLines} positions -> ${groups.length} rotation classes`, karn: `Grouping bucket ${b + 1}/${n}: ${bucketLines} positions -> ${groups.length} rotation classes`, isSolution: false });
          await solveGroupList(groups);
        }
        if (!batchStopRef.current && b < n - 1) {
          const progress = `Progress: ${batchSolvedRef.current}/${totalLines} solved (bucket ${b + 1}/${n})`;
          addOutputLine({ raw: progress, karn: progress, isSolution: false });
        }
      }
    };
    try {
      setStatusLines(["Initializing solver..."]);
      await native.core.invoke("batch_init", { flags });
      setStatusLines([]);
      if (stored) {
        // Stream the input out of storage in reasonably big chunks: read a
        // chunk into memory, solve it, discard, then pull the next chunk.
        const chunkCount = batchInputChunkCountRef.current;
        if (enableGrouping) {
          await groupedBucketSolve(chunkCount);
        } else {
          for (let ci = 0; ci < chunkCount; ci++) {
            if (batchStopRef.current) break;
            const chunk = (await readBatchInputChunk(ci)) ?? [];
            if (!chunk.length) continue;
            await processChunk(chunk);
            if (ci % BATCH_PROGRESS_EVERY === BATCH_PROGRESS_EVERY - 1 || ci === chunkCount - 1) {
              const progress = `Progress: ${batchSolvedRef.current}/${totalLines} solved (chunk ${ci + 1}/${chunkCount})`;
              addOutputLine({ raw: progress, karn: progress, isSolution: false });
            }
          }
        }
      } else {
        await processChunk(batchNonEmptyLines(batchInput));
      }
    } finally {
      try { await native.core.invoke("batch_destroy"); } catch { /* ignore */ }
      // Always clear the running flags, including when the batch bails early
      // (e.g. a worker terminate on stop) — otherwise the main "running" state
      // stays true and the whole UI remains disabled with no way out.
      batchRunningRef.current = false;
      setBatchRunning(false);
      setRunningState(false);
    }
    if (runId !== solveRunId.current) return;
    if (batchProgressFrameRef.current !== undefined) {
      cancelAnimationFrame(batchProgressFrameRef.current);
      batchProgressFrameRef.current = undefined;
    }
    setBatchSolved(batchSolvedRef.current);
    // Fold any pending tail into disk so RAM only keeps the current page.
    await syncOffload();
    flushSolutionState();
    const solvedCount = batchSolvedRef.current;
    const errorCount = totalLines - solvedCount;
    const groupCount = totalCountRefs();
    const elapsed = ((performance.now() - startedAt) / 1000).toFixed(2);
    const statusMsg = `Done: ${solvedCount} solved, ${errorCount} errors, ${totalLines} total (${groupCount} groups, ${elapsed}s)`;
    setStatusLines([statusMsg]);
  };
  const batchRunningRef = useRef(false);
  const generateBatchStats = async () => {
    if (!totalCount) return;
    const counts = new Map<number, number>();
    let solvedCount = 0, groups = 0, totalMetric = 0;
    await forEachSolution((row) => {
      const freq = row.frequency ?? 1;
      const s = batchMetricKey === "moves" ? (row.moves || 0) : batchMetricKey === "angle" ? (row.angle || 0) : (row.slices || 0);
      counts.set(s, (counts.get(s) ?? 0) + freq);
      solvedCount += freq;
      groups++;
      totalMetric += s * freq;
    });
    if (!groups) return;
    const maxVal = Math.max(0, ...counts.keys());
    const lines: string[] = [];
    for (let s = 0; s <= maxVal; s++) {
      lines.push(`${s} ${batchMetricLabel} solutions: ${counts.get(s) ?? 0}`);
    }
    if (solvedCount) {
      lines.push(`average ${batchMetricLabel} count: ${(totalMetric / solvedCount).toFixed(2)}`);
    }
    const batchTotalLines = batchStoredRef.current ? batchStoredLinesRef.current : batchLineCount(batchInput);
    const errorCount = batchTotalLines - solvedCount;
    lines.push(`errors: ${errorCount}`);
    lines.push(`groups: ${groups} (${solvedCount} inputs)`);
    setBatchStatsText(lines.join("\n"));
  };
  const downloadBatchCSV = async () => {
    if (!totalCount) return;
    const csvRows: string[] = [`input,solution,${batchMetricLabel}`];
    await forEachSolution((row) => {
      if (row.input === undefined || row.solution === undefined) return;
      const metricVal = batchMetricKey === "moves" ? (row.moves || 0) : batchMetricKey === "angle" ? (row.angle || 0) : (row.slices || 0);
      csvRows.push(`${row.input},${row.solution.replace(/,/g, " ")},${metricVal}`);
    });
    const csv = csvRows.join("\n");
    const blob = new Blob([csv], { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "batch_solutions.csv";
    a.click();
    URL.revokeObjectURL(url);
    setStatusLines(["CSV downloaded"]);
  };
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.ctrlKey && event.key === "Enter") {
        event.preventDefault();
        void solve();
      }
      if (event.ctrlKey && (event.key === "=" || event.key === "+")) {
        event.preventDefault();
        setZoom((value) => {
          const next = Math.min(2, Math.round((value + 0.1) * 10) / 10);
          zoomRef.current = next;
          document.documentElement.classList.toggle("tall-viewport", window.innerHeight / next >= readBreakpoints().tall);
          return next;
        });
      }
      if (event.ctrlKey && event.key === "-") {
        event.preventDefault();
        setZoom((value) => {
          const next = Math.max(0.5, Math.round((value - 0.1) * 10) / 10);
          zoomRef.current = next;
          document.documentElement.classList.toggle("tall-viewport", window.innerHeight / next >= readBreakpoints().tall);
          return next;
        });
      }
      if (event.ctrlKey && event.key === "0") {
        event.preventDefault();
        zoomRef.current = 1;
        setZoom(1);
        document.documentElement.classList.toggle("tall-viewport", window.innerHeight >= readBreakpoints().tall);
      }
      if (event.altKey && event.shiftKey && (event.key === "P" || event.key === "p")) {
        event.preventDefault();
        setResearchMode((v) => !v);
      }
      if (event.ctrlKey && event.key.toLowerCase() === "z") { event.preventDefault(); doUndo(); }
      if (event.ctrlKey && event.key.toLowerCase() === "y") { event.preventDefault(); doRedo(); }
      if (event.key === "Escape") {
        // Closes only the topmost thing open, in nested order.
        if (stickyTooltip) { event.preventDefault(); setStickyTooltip(null); return; }
        if (contextMenu) { event.preventDefault(); setContextMenu(null); return; }
        if (openDropdown) {
          event.preventDefault();
          if (document.activeElement instanceof HTMLElement && document.activeElement.closest(".option-dropdown")) {
            document.activeElement.blur();
          }
          setOpenDropdown(null);
          return;
        }
        if (modeMenu) { event.preventDefault(); setModeMenu(false); return; }
        if (menu) { event.preventDefault(); setMenu(false); return; }
        if (showAllConfirm) { event.preventDefault(); setShowAllConfirm(false); return; }
        if (diskOpen) { event.preventDefault(); setDiskOpen(false); return; }
        if (weightsOpen) { event.preventDefault(); setWeightsOpen(false); return; }
        if (favoritesOpen) { event.preventDefault(); beginCloseFavorites(); return; }
        if (modal) { event.preventDefault(); history.back(); return; }
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  });
  useEffect(() => {
    void loadSettings().then((value) => {
      if (!value) { queueMicrotask(() => { settingsReady.current = true; }); return; }
      if (typeof value.outputMode === "string") {
        const restored = value.outputMode === "normal" ? "default" : value.outputMode;
        if (OUTPUT_MODES.includes(restored as OutputMode)) setOutputMode(restored as OutputMode);
      } else if (typeof value.smartKarn === "boolean" && typeof value.karn === "boolean") setOutputMode(value.smartKarn ? "cskarn" : value.karn ? "karn" : "default");
      else if (typeof value.karn === "boolean") setOutputMode(value.karn ? "karn" : "default");
      if (typeof value.abidNotation === "boolean") setAbidNotation(value.abidNotation);
      if (typeof value.ignoreTransforms === "boolean") setIgnoreTransforms(value.ignoreTransforms);
      if (typeof value.debugOutput === "boolean") setDebugOutput(value.debugOutput);
      if (typeof value.normalize === "string") setNormalize(LEGACY_NORMALIZE[value.normalize] ?? "none");
      if (typeof value.mode === "string") setMode(value.mode);
      if (typeof value.metric === "string") setMetric(LEGACY_METRIC[value.metric] ?? "es");
      if (typeof value.two === "string") setTwo(LEGACY_TWO[value.two] ?? "none");
      if (typeof value.angle === "string") setAngle(LEGACY_ANGLE[value.angle] ?? "none");
      if (typeof value.all === "boolean") setAll(value.all);
      if (typeof value.suboptimal === "number") setSuboptimal(value.suboptimal);
      if (typeof value.depths === "string") setDepths(value.depths);
      if (typeof value.generator === "boolean") setGenerator(value.generator);
      if (typeof value.cubeShape === "boolean") setCubeShapeMemory(value.cubeShape);
      if (typeof value.ignoreMiddle === "boolean") {
        setIgnoreMiddle(value.ignoreMiddle);
        if (value.ignoreMiddle) queueMicrotask(() => { ignoreHistory.current = true; toggleIgnoreMiddle(true); ignoreHistory.current = false; });
      }
      if (typeof value.maxX === "boolean") setMaxX(value.maxX);
      if (typeof value.maxXValue === "number") setMaxXValue(value.maxXValue);
      if (typeof value.maxY === "boolean") setMaxY(value.maxY);
      if (typeof value.maxYValue === "number") setMaxYValue(value.maxYValue);
      if (typeof value.maxTotal === "boolean") setMaxTotal(value.maxTotal);
      if (typeof value.maxTotalValue === "number") setMaxTotalValue(value.maxTotalValue);
      if (typeof value.zoom === "number") { setZoom(value.zoom); zoomRef.current = value.zoom; document.documentElement.classList.toggle("tall-viewport", window.innerHeight / value.zoom >= readBreakpoints().tall); }
      if (typeof value.pageSize === "number" && PAGE_SIZE_OPTIONS.includes(value.pageSize)) setPageSize(value.pageSize);
      if (typeof value.showAll === "boolean") setShowAll(value.showAll);
      if (typeof value.useLessRam === "boolean") setUseLessRam(value.useLessRam);
      if (typeof value.deleteTablesOnQuitV2 === "boolean") setDeleteTablesOnQuit(value.deleteTablesOnQuitV2);
      if (value.weightOverrides && typeof value.weightOverrides === "object") setWeightOverrides(value.weightOverrides as Partial<{ w1: number; w2: number; w3: number; w4: number }>);
      if (value.moveValueOverrides && typeof value.moveValueOverrides === "object") setMoveValueOverrides(value.moveValueOverrides as Record<string, number>);
      queueMicrotask(() => { settingsReady.current = true; });
    });
  }, []);
  useEffect(() => {
    if (!settingsReady.current) return;
    void saveSettings({
      outputMode, abidNotation, ignoreTransforms, debugOutput, normalize, mode,
      metric, two, angle, all, suboptimal, depths, generator, cubeShape: cubeShapeMemory, ignoreMiddle,
      maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue, zoom, pageSize, showAll, useLessRam,
      deleteTablesOnQuitV2, weightOverrides, moveValueOverrides,
    });
  }, [outputMode, abidNotation, ignoreTransforms, debugOutput, normalize, mode, metric, two, angle, all, suboptimal, depths, generator, cubeShapeMemory, ignoreMiddle, maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue, zoom, pageSize, showAll, useLessRam, deleteTablesOnQuit, weightOverrides, moveValueOverrides]);
  useEffect(() => { debugOutputRef.current = debugOutput; }, [debugOutput]);
  useEffect(() => {
    // Pushes the current ergonomics-rater overrides to the native/web solver
    // backend so they take effect on the very next rating, including on
    // startup once any overrides are restored from storage above.
    const native = tauri();
    if (!native?.core?.invoke) return;
    const weights: [number, number, number, number] = [
      weightOverrides.w1 ?? DEFAULT_WEIGHTS.w1,
      weightOverrides.w2 ?? DEFAULT_WEIGHTS.w2,
      weightOverrides.w3 ?? DEFAULT_WEIGHTS.w3,
      weightOverrides.w4 ?? DEFAULT_WEIGHTS.w4,
    ];
    void native.core.invoke("set_rating_config", { weights, moveValues: moveValueOverrides }).catch(() => undefined);
  }, [weightOverrides, moveValueOverrides]);
  useEffect(() => {
    if (!solutions.length || running) return;
    let cancelled = false;
    void (async () => {
      for (const solution of solutions) {
        const pair = await buildDisplayPair(solution.raw, lastSolvePosition.current || positionString(cubeState), solution.sliceStart);
        if (cancelled) return;
        const rebuilt = solutionsRef.current.map((row) => row.raw === solution.raw ? { ...row, ...pair } : row);
        setSolutionRows(rebuilt);
        replaceOutputLines(outputLinesRef.current.map((entry) => entry.algRaw === solution.algRaw ? { ...entry, raw: pair.rawDisplay, karn: pair.karnDisplay } : entry));
      }
    })();
    return () => { cancelled = true; };
    // Rebuild only when karnification inputs change, not when solution rows arrive.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [outputMode, generator]);
  useEffect(() => {
    let cancelled = false;
    if (!tauri()?.core?.invoke) return;
    const specificAngleBot = angle === "nb" || angle === "nd";
    void tauri()!.core!.invoke<TwoGenStatus>("two_gen_status", { position: rawPosition(cubeState), specificAngleBot })
      .then((status) => { if (!cancelled) setTwoGenStatus(status); })
      .catch((err) => console.error("two_gen_status invoke failed:", err));
    return () => { cancelled = true; };
  }, [cubeState, angle]);
  useEffect(() => {
    void loadFavorites().then((value) => {
      if (value) setFavorites(value);
      queueMicrotask(() => { favoritesReady.current = true; });
    });
  }, []);
  useEffect(() => {
    if (!favoritesReady.current) return;
    void saveFavorites(favorites);
  }, [favorites]);
  useEffect(() => {
    let unlisten: (() => void) | undefined;
    const cleanupOnQuit = () => {
      void clearOffloadedSolutions();
      if (deleteTablesOnQuitRef.current) {
        if (isNativePlatform()) void clearAllTables();
        else void clearTableBlobs();
      }
    };
    if ((window as Window & { __TAURI__?: unknown }).__TAURI__) {
      void import("@tauri-apps/api/window").then(async ({ getCurrentWindow }) => {
        try {
          unlisten = await getCurrentWindow().onCloseRequested(() => {
            cleanupOnQuit();
          });
        } catch { /* capability missing */ }
      });
    } else {
      const onBeforeUnload = () => { cleanupOnQuit(); };
      window.addEventListener("beforeunload", onBeforeUnload);
      return () => { window.removeEventListener("beforeunload", onBeforeUnload); };
    }
    return () => { unlisten?.(); };
  }, []);
  const cubeshapeBlockedBy2Gen = (two === "2g" && !twoGenStatus.cornersTwo) ||
    (two === "p2g" && !twoGenStatus.cornersPseudo);
  const cubeshapeForced = !isGoodSquares(cubeState) || cubeshapeBlockedBy2Gen;
  const cubeShape = cubeShapeMemory && !cubeshapeForced;
  const twoGenBlocked = (two === "2g" && (twoGenStatus.compatibility < 2 || (cubeShape && !twoGenStatus.cornersTwo))) ||
    (two === "p2g" && (twoGenStatus.compatibility < 1 || (cubeShape && !twoGenStatus.cornersPseudo)));
  const cubeshapeDisableReason = cubeshapeBlockedBy2Gen
    ? t('errors.no2GenCorners')
    : !inCubeshape(cubeState) ? (() => {
      const rTop = getLayerR(cubeState.position, 0), rBot = getLayerR(cubeState.position, 12);
      if (rTop < 0 || rBot < 0 || rTop === 1 || rBot === 1) return t('errors.notCubeshape');
      if (cubeState.partial.every((v) => v === 0)) {
        const odd = getParityOdd(cubeState.position);
        if ((rTop === rBot) !== odd) return t('errors.badParity');
      }
      return t('errors.notCubeshape');
    })() : null;
  const specificDepthsActive = depths.trim().length > 0;
  const commandFlags = solverFlags({ metric, all, suboptimal, depths, generator, two, cubeshape: cubeShape, ignoreEquator: ignoreMiddle, angle, maxX, maxXValue, maxY, maxYValue, maxTotal, maxTotalValue });
  if (ignoreTransforms) commandFlags.push("-x");
  if (smartKarn) commandFlags.push("-k2");
  if (outputMode === "karn") commandFlags.push("-k1");
  if (outputMode === "abid") commandFlags.push("-k3");
  const commandPreview = `croissant ${commandFlags.join(" ")} ${positionString(cubeState)}`;
  const showErgo = runCubeShape;
  const solutionDisplayText = (solution: Solution) =>
    solution.rawPassThrough ? solution.rawDisplay : buildDisplayText(solution.rawDisplay, solution.karnDisplay, solution.abidDisplay, solution.sliceStart);
  const displaySolution = (solution: Solution): DisplaySolution => {
    const display = solutionDisplayText(solution);
    return { ...solution, display, alg: lineAlg(display) };
  };
  // Abbreviates large counts to 3 sig figs with a k/m/b suffix, trimming trailing zeros
  const formatCount = (n: number): string => {
    const units: [number, string][] = [[1e9, "b"], [1e6, "m"], [1e3, "k"]];
    for (const [value, suffix] of units) {
      if (n >= value) {
        const scaled = n / value;
        const digits = scaled >= 100 ? 0 : scaled >= 10 ? 1 : 2;
        return `${scaled.toFixed(digits).replace(/\.?0+$/, "")}${suffix}`;
      }
    }
    return `${n}`;
  };
  // Scans all solutions (including offloaded/low-RAM chunks) and matches
  // against the displayed alg text (never the abid-barred glyphs) so the filter
  // covers everything, not just the currently loaded page.
  const runFilterSearch = async (query: string, id: number) => {
    let matcher: (alg: string) => boolean;
    if (filterRegexMode) {
      let re: RegExp;
      try {
        re = new RegExp(query, filterMatchCase ? "" : "i");
      } catch {
        if (id !== filterSearchIdRef.current) return;
        setFilterInvalid(true);
        setFilterResults([]);
        setFilterAppliedQuery(query);
        setFilterSearching(false);
        setPage(0);
        return;
      }
      matcher = (alg) => re.test(alg);
    } else {
      const useCommaTerms = outputMode !== "default" && outputMode !== "wca";
      if (useCommaTerms) {
        const terms = query.split(",").map((t) => t.trim()).filter((t) => t.length > 0);
        const needles = terms.map((t) => filterMatchCase ? t : t.toLowerCase());
        matcher = (alg) => {
          const hay = filterMatchCase ? alg : alg.toLowerCase();
          return needles.some((n) => hay.includes(n));
        };
      } else {
        const needle = filterMatchCase ? query : query.toLowerCase();
        matcher = (alg) => (filterMatchCase ? alg : alg.toLowerCase()).includes(needle);
      }
    }
    if (filterNegate) {
      const positiveMatch = matcher;
      matcher = (alg) => !positiveMatch(alg);
    }
    const seen = new Set<string>();
    const matches: Solution[] = [];
    const consider = (solution: Solution) => {
      const alg = displaySolution(solution).alg;
      if (seen.has(alg)) return;
      seen.add(alg);
      if (matcher(alg)) matches.push(solution);
    };
    if (offloadActive()) {
      const chunks = [...offloadedChunksRef.current.entries()].sort((a, b) => a[0] - b[0]);
      for (const [start] of chunks) {
        if (id !== filterSearchIdRef.current) return;
        const raw = await readOffloadedChunk(start);
        if (raw) for (const s of raw) consider(s);
      }
      for (const s of pendingTailRef.current ?? []) consider(s);
    }
    for (const solution of solutionsRef.current) consider(solution);
    if (id !== filterSearchIdRef.current) return;
    setFilterInvalid(false);
    setFilterResults(matches);
    setFilterAppliedQuery(query);
    setFilterSearching(false);
    setPage(0);
  };
  useEffect(() => {
    if (!filterOpen) return;
    if (!filterQuery.trim()) {
      filterSearchIdRef.current++;
      setFilterResults(null);
      setFilterAppliedQuery("");
      setFilterSearching(false);
      setFilterInvalid(false);
      return;
    }
    setFilterSearching(true);
    setFilterInvalid(false);
    const id = ++filterSearchIdRef.current;
    // Delay the actual scan until typing settles, since it can walk the entire
    // (potentially offloaded) result set.
    const timer = window.setTimeout(() => { void runFilterSearch(filterQuery, id); }, 400);
    return () => window.clearTimeout(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [filterOpen, filterQuery, filterMatchCase, filterRegexMode, filterNegate, outputMode, normalize]);
  // Pagination targets the filtered set (already fully materialized in memory
  // by runFilterSearch) rather than the raw total when a filter is active.
  const activeTotalCount = filterActive ? filterResults!.length : totalCount;
  const totalPages = showAll ? 1 : (() => {
    const raw = Math.max(1, Math.ceil(activeTotalCount / pageSize));
    // Intentional feature by Abid: fold a tiny trailing overflow (<10% of a page)
    // into the previous page instead of creating a nearly-empty last page.
    const remainder = activeTotalCount % pageSize;
    return remainder > 0 && remainder < pageSize * 0.1 ? Math.max(1, raw - 1) : raw;
  })();
  const clampedPage = Math.min(page, totalPages - 1);
  const pageStart = clampedPage * pageSize;
  const pageEnd = showAll ? activeTotalCount : clampedPage === totalPages - 1 ? activeTotalCount : Math.min(pageStart + pageSize, activeTotalCount);
  useEffect(() => {
    if (pageInputFocused.current) return;
    setPageInput(String(clampedPage + 1));
  }, [clampedPage]);
  const pageSolutions = (() => {
    if (filterActive) {
      // filterResults is already deduplicated by alg during the search scan.
      const rows: DisplaySolution[] = [];
      for (let i = pageStart; i < pageEnd; i++) {
        const solution = filterResults![i];
        if (solution) rows.push(displaySolution(solution));
      }
      return rows;
    }
    const seen = new Set<string>();
    const rows: DisplaySolution[] = [];
    const ramOffset = offloadActive() ? ramOffsetRef.current : 0;
    for (let i = pageStart; i < pageEnd; i++) {
      const index = i - ramOffset;
      if (index < 0 || index >= solutions.length) continue;
      const row = displaySolution(solutions[index]);
      if (seen.has(row.alg)) continue;
      seen.add(row.alg);
      rows.push(row);
    }
    return rows;
  })();
  const collectAllDisplays = async () => {
    const seen = new Set<string>();
    const out: string[] = [];
    const add = (solution: Solution) => {
      const display = solutionDisplayText(solution);
      const alg = lineAlg(display);
      if (seen.has(alg)) return;
      seen.add(alg);
      out.push(display);
    };
    if (offloadActive()) {
      const chunks = [...offloadedChunksRef.current.entries()].sort((a, b) => a[0] - b[0]);
      for (const [start] of chunks) {
        const raw = await readOffloadedChunk(start);
        if (raw) for (const s of raw) add(s);
      }
      for (const s of pendingTailRef.current ?? []) add(s);
    }
    for (const solution of solutionsRef.current) add(solution);
    return out;
  };
  const displayErgo = (solution: Solution) =>
    running || tableBusyMessage ? solutionErgo(solution) ?? solution.ergoRaw : solutionErgo(solution);
  const tableSolutions = [...pageSolutions].sort((a, b) => {
    if (showErgo && pageSolutions.some((row) => displayErgo(row) !== undefined)) {
      const aErgo = displayErgo(a), bErgo = displayErgo(b);
      const aNan = aErgo === undefined, bNan = bErgo === undefined;
      if (aNan && !bNan) return 1;
      if (!aNan && bNan) return -1;
      if (!aNan && !bNan && aErgo !== bErgo) return bErgo - aErgo;
    }
    if (a.slices !== b.slices) return a.slices - b.slices;
    return a.moves - b.moves;
  });
  const terminalSolutions = pageSolutions.map((solution, index) => {
    const ergo = displayErgo(solution);
    const suffix = showErgo && !running
      ? ergo === undefined ? "  (⚠)" : `  (${ergo.toFixed(2)})`
      : "";
    return { key: `sol-${solution.raw}-${index}`, text: solution.display + suffix, solution };
  });
  const outputLineText = (entry: OutputLine) => entry.isSolution ? notationStyle(karn ? entry.karn : entry.raw, outputMode, !!entry.sliceStart) : entry.raw;
  const notationCleanClass = outputMode === "clean" ? "notation-clean" : "";
  const terminalNonSolutions = outputLines
    .filter((entry) => !entry.isSolution)
    .map((entry, index) => ({ key: `line-${index}-${entry.raw}`, text: outputLineText(entry) }));
  const copyTerminalText = () => {
    const lines = outputLines.map(outputLineText);
    void navigator.clipboard.writeText(lines.join("\n"));
    setStatusLines((old) => [...old, t('terminal.copied')].slice(-8));
  };
  // literal-mode search term highlighting inside solution lines (not regex, not negate)
  const highlightActive = filterActive && !filterRegexMode && !filterNegate && filterAppliedQuery.trim() !== "";
  const highlightQuery = (text: string, query: string, matchCase: boolean) => {
    const useCommaTerms = outputMode !== "default" && outputMode !== "wca";
    const terms = useCommaTerms
      ? query.split(",").map((t) => t.trim()).filter((t) => t.length > 0)
      : [query];
    const hay = matchCase ? text : text.toLowerCase();
    const allMatches: { start: number; end: number; termIdx: number }[] = [];
    for (let ti = 0; ti < terms.length; ti++) {
      const needle = matchCase ? terms[ti] : terms[ti].toLowerCase();
      if (!needle) continue;
      let idx = hay.indexOf(needle, 0);
      while (idx !== -1) {
        allMatches.push({ start: idx, end: idx + needle.length, termIdx: ti });
        idx = hay.indexOf(needle, idx + needle.length);
      }
    }
    if (allMatches.length === 0) return text;
    allMatches.sort((a, b) => a.start - b.start || a.termIdx - b.termIdx);
    // Merge overlapping spans; keep the earliest-starting term's color for each
    // merged region but split if a later region starts inside but ends outside.
    const regions: { start: number; end: number; termIdx: number }[] = [];
    for (const m of allMatches) {
      const last = regions[regions.length - 1];
      if (last && m.start <= last.end) {
        if (m.end > last.end) {
          regions.push({ start: last.end, end: m.end, termIdx: m.termIdx });
          last.end = m.start < last.end ? m.start : last.end;
        }
      } else {
        regions.push({ start: m.start, end: m.end, termIdx: m.termIdx });
      }
    }
    // Re-merge adjacent/overlapping same-term spans
    const merged: { start: number; end: number; termIdx: number }[] = [];
    for (const r of regions) {
      const last = merged[merged.length - 1];
      if (last && last.termIdx === r.termIdx && r.start <= last.end) {
        last.end = Math.max(last.end, r.end);
      } else {
        merged.push({ start: r.start, end: r.end, termIdx: r.termIdx });
      }
    }
    const parts: (string | JSX.Element)[] = [];
    let cursor = 0;
    for (const m of merged) {
      if (m.start > cursor) parts.push(text.slice(cursor, m.start));
      const cls = `filter-highlight filter-hl-${m.termIdx % 6}`;
      parts.push(<mark className={cls} key={`${m.start}-${m.termIdx}`}>{text.slice(m.start, m.end)}</mark>);
      cursor = m.end;
    }
    if (cursor < text.length) parts.push(text.slice(cursor));
    return parts;
  };
  const renderSolutionText = (text: string) => {
    const showBarred = outputMode === "abid" || (karn && abidNotation);
    if (!showBarred) return highlightActive ? highlightQuery(text, filterAppliedQuery, filterMatchCase) : text;
    const lb = text.lastIndexOf("[");
    if (lb <= 0) return <span className="abid-inline">{abidify(text)}</span>;
    return <><span className="abid-inline">{abidify(text.slice(0, lb).trim())}</span>{"  " + text.slice(lb).trim()}</>;
  };
  const tableBusyMessages = [
    tableBusyMessage,
    t('table.busyResolving'),
    t('table.busyRating'),
    t('table.busyNormalizing'),
    t('table.busyBuilding'),
  ].filter((message, index, all) => message && all.indexOf(message) === index);
  const tableBusyText = tableBusyMessages[tableBusyTick % tableBusyMessages.length] || tableBusyMessage;
  const updateOptionalLimit = (raw: string, min: number, max: number, setEnabled: (value: boolean) => void, setValue: (value: number) => void) => {
    if (raw.trim() === "") {
      setEnabled(false);
      return;
    }
    const parsed = Number(raw);
    if (!Number.isFinite(parsed)) return;
    // The true maximum (6 or 12) lives in the empty placeholder state, so any
    // typed value above the accessible range falls back into that placeholder.
    if (parsed > max) {
      setEnabled(false);
      return;
    }
    setEnabled(true);
    setValue(Math.max(min, Math.trunc(parsed)));
  };
  const stepOptionalLimit = (enabled: boolean, value: number, dir: 1 | -1, min: number, max: number, setEnabled: (value: boolean) => void, setValue: (value: number) => void) => {
    // The empty placeholder mode represents the value one above max (6 or 12),
    // and wraps the stepper range: up from max → placeholder → min, and
    // down from min → placeholder → max.
    if (!enabled) {
      setEnabled(true);
      setValue(dir === 1 ? min : max);
      return;
    }
    if (dir === 1) {
      if (value >= max) {
        setEnabled(false);
      } else {
        setValue(value + 1);
      }
    } else if (value <= min) {
      setEnabled(false);
    } else {
      setValue(value - 1);
    }
  };
  const renderOptionsPanel = () => (
    <div className="options-panel" ref={optionsPanelRef}>
      <div className="mobile-modal-head">
        <b>{t('options.heading')}</b>
        <button aria-label={t('btn.closeOptions')} onClick={() => setMobileOptionsOpen(false)}><Icon name="close" /></button>
      </div>
      <h2>{t('options.heading')}</h2>
      <div className="select-grid">
        <OptionDropdown id="metric" label={t('options.metric')} title={tooltips.metric} value={metric} options={[{ value: "es", label: tList('options.metricValues')[0] }, { value: "move", label: tList('options.metricValues')[1] }, { value: "ea", label: tList('options.metricValues')[2] }]} disabled={running} open={openDropdown === "metric"} setOpen={setOpenDropdown} onChange={setMetric} />
        <OptionDropdown id="two" label={t('options.twoGen')} title={tooltips.twoGen} value={two} options={[{ value: "none", label: tList('options.twoGenValues')[0] }, { value: "p2g", label: tList('options.twoGenValues')[1] }, { value: "2g", label: tList('options.twoGenValues')[2] }]} disabled={running} highlight open={openDropdown === "two"} setOpen={setOpenDropdown} onChange={setTwo} />
        <OptionDropdown id="angle" label={t('options.lockLayer')} title={tooltips.angle} value={angle} options={[{ value: "none", label: tList('options.lockValues')[0] }, { value: "nb", label: tList('options.lockValues')[1] }, { value: "nu", label: tList('options.lockValues')[2] }, { value: "nd", label: tList('options.lockValues')[3] }]} disabled={running} highlight open={openDropdown === "angle"} setOpen={setOpenDropdown} onChange={setAngle} />
        <OptionDropdown id="normalize" label={t('options.normalizeABF')} title={tooltips.normalize} value={normalize} options={[{ value: "none", label: tList('options.normalizeValues')[0] }, { value: "both", label: tList('options.normalizeValues')[1] }, { value: "pre", label: tList('options.normalizeValues')[2] }, { value: "post", label: tList('options.normalizeValues')[3] }]} disabled={running} highlight open={openDropdown === "normalize"} setOpen={setOpenDropdown} onChange={setNormalize} />
      </div>
      <div className="check-grid">
        <span className="generator-toggle">{t('options.output')} <span className="generator-toggle-value" title={tooltips.generator} onClick={() => !running && setGenerator((g) => !g)}>{generator ? t('options.outputValueScramble') : t('options.outputValueSolution')}</span></span>
        <label className="inline-all-optimal" title={tooltips.all}>
          <input type="checkbox" checked={all} disabled={running} onChange={(e) => setAll(e.target.checked)} />
          <span>{t('options.generateAll')}</span>
          <span className="all-optimal-label">{suboptimal && !specificDepthsActive ? `${t('options.optimal')}+${suboptimal}` : t('options.optimal')}</span>
          {!specificDepthsActive && <span className="stepper-group">
            <button type="button" title={tooltips.suboptimal} disabled={running || !all} onClick={() => setSuboptimal((value) => Math.max(0, value - 1))}>−</button>
            <button type="button" title={tooltips.suboptimal} disabled={running || !all} onClick={() => setSuboptimal((value) => value + 1)}>+</button>
          </span>}
        </label>
        <label title={cubeshapeDisableReason ?? tooltips.cubeshape}><input type="checkbox" checked={cubeShape} disabled={running || !isGoodSquares(cubeState) || cubeshapeBlockedBy2Gen} onChange={(e) => setCubeShapeMemory(e.target.checked)} /> {t('options.stayInCS')}</label>
      </div>
      <div className="limit-grid">
        <label title={tooltips.maxX}>{t('options.maxTop')}
          <div className="number-input-wrap">
            <input type="number" min="0" max="5" value={maxX ? maxXValue : ""} placeholder="6" disabled={running} onChange={(e) => updateOptionalLimit(e.target.value, 0, 5, setMaxX, setMaxXValue)} />
            <div className="number-stepper">
              <button type="button" className="top-stepper" title={tooltips.maxX} disabled={running} onClick={() => stepOptionalLimit(maxX, maxXValue, 1, 0, 5, setMaxX, setMaxXValue)}>▲</button>
              <button type="button" className="bottom-stepper" title={tooltips.maxX} disabled={running} onClick={() => stepOptionalLimit(maxX, maxXValue, -1, 0, 5, setMaxX, setMaxXValue)}>▼</button>
            </div>
          </div>
        </label>
        <label title={tooltips.maxY}>{t('options.maxBottom')}
          <div className="number-input-wrap">
            <input type="number" min="0" max="5" value={maxY ? maxYValue : ""} placeholder="6" disabled={running} onChange={(e) => updateOptionalLimit(e.target.value, 0, 5, setMaxY, setMaxYValue)} />
            <div className="number-stepper">
              <button type="button" className="top-stepper" title={tooltips.maxY} disabled={running} onClick={() => stepOptionalLimit(maxY, maxYValue, 1, 0, 5, setMaxY, setMaxYValue)}>▲</button>
              <button type="button" className="bottom-stepper" title={tooltips.maxY} disabled={running} onClick={() => stepOptionalLimit(maxY, maxYValue, -1, 0, 5, setMaxY, setMaxYValue)}>▼</button>
            </div>
          </div>
        </label>
        <label title={tooltips.maxTotal}>{t('options.maxTotal')}
          <div className="number-input-wrap">
            <input type="number" min="2" max="11" value={maxTotal ? maxTotalValue : ""} placeholder="12" disabled={running} onChange={(e) => updateOptionalLimit(e.target.value, 2, 11, setMaxTotal, setMaxTotalValue)} />
            <div className="number-stepper">
              <button type="button" className="top-stepper" title={tooltips.maxTotal} disabled={running} onClick={() => stepOptionalLimit(maxTotal, maxTotalValue, 1, 2, 11, setMaxTotal, setMaxTotalValue)}>▲</button>
              <button type="button" className="bottom-stepper" title={tooltips.maxTotal} disabled={running} onClick={() => stepOptionalLimit(maxTotal, maxTotalValue, -1, 2, 11, setMaxTotal, setMaxTotalValue)}>▼</button>
            </div>
          </div>
        </label>
        <label title={tooltips.depths}>{t('options.specificDepths')}<input type="text" value={depths} disabled={running} onChange={(e) => /^\s*\d*(?:\s*,\s*\d*)*\s*$/.test(e.target.value) && setDepths(e.target.value)} placeholder={t('options.placeholderDepth')} /></label>
      </div>
    </div>
  );
  const computeDebugStats = () => {
    const now = performance.now();
    const start = solveStartTimeRef.current;
    if (!start) return { elapsed: "—", solutionCount: 0, nodesSearched: 0 };
    const end = runningRef.current ? now : (solveStopTimeRef.current || now);
    const elapsed = ((end - start) / 1000).toFixed(1);
    const solutionCount = totalCountRefs();
    const nodesSearched = progressNodesRef.current;
    return { elapsed, solutionCount, nodesSearched };
  };
  const renderOutputShell = () => {
    const tableCols = tableView ? computeTableCols(tableWidth, tableMetricRef.current, showErgo, document.documentElement.classList.contains("bp-720")) : null;
    return (
    <div className="terminal-shell">
      <div className="output-tools">
        <div className="output-tools-left">
          {researchMode ? <span className="generator-toggle">Batch: {formatCount(batchSolved)} solved</span> : <span className="generator-toggle">{t('outputNotation')} <span className="generator-toggle-value" title={tooltips.karn} onClick={() => !running && setModal("notation")}>{t('karnSelect.' + outputMode)}</span></span>}
        </div>
        <div className="output-tools-right">
          {researchMode && <>
            <button title="Download CSV" disabled={!batchSolved} onClick={() => void downloadBatchCSV()}><Icon name="copy" /></button>
            <button title="Show Stats" disabled={!batchSolved} onClick={() => void generateBatchStats()}>S</button>
          </>}
          {!researchMode && debugOutput && <button title={t('btn.debugStats')} onClick={() => setModal("debug")}><Icon name="timer" /></button>}
          {!researchMode && <button
            className={`filter-trigger ${filterOpen || filterActive ? "active" : ""}`}
            title={t('filter.find')}
            disabled={running || !totalCount}
            onClick={() => setFilterOpen((v) => !v)}
          ><Icon name="search" /></button>}
          {!researchMode && <button title={t('btn.copyAll')} disabled={!totalCount} onClick={copyTerminalText}><Icon name="copy" /></button>}
          {!researchMode && <button title={tableView ? t('btn.switchTerminalView') : t('btn.switchTableView')} onClick={() => tableView ? switchToTerminalMode() : switchToTableMode()}><Icon name={tableView ? "list" : "grid"} /></button>}
          {running && <button className="terminal-stop" title={t('btn.stop')} aria-label={t('btn.stop')} onClick={() => void solve()}><Icon name="stop" /></button>}
          {batchRunning && <button className="terminal-stop" title="Stop batch" aria-label="Stop batch" onClick={() => { batchStopRef.current = true; const native = tauri(); native?.core?.invoke("stop_solver").catch(() => undefined); }}><Icon name="stop" /></button>}
          <button className="mobile-output-close" title={t('btn.close')} aria-label={t('btn.close')} onClick={() => setMobileOutputOpen(false)}><Icon name="close" /></button>
          <button className="expand-output" title={expanded ? t('btn.shrinkTerminal') : t('btn.expandTerminal')} onClick={() => setExpanded((v) => !v)}><Icon name={expanded ? "collapse" : "expand"} /></button>
        </div>
      </div>
      {filterOpen && <div className="filter-overlay" role="search">
        <input
          ref={filterInputRef}
          type="text"
          className="filter-input"
          placeholder={t('filter.find')}
          value={filterQuery}
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="off"
          onChange={(event) => setFilterQuery(event.target.value)}
          onKeyDown={(event) => { if (event.key === "Escape") { event.preventDefault(); setFilterOpen(false); } }}
        />
        <span className={`filter-count ${filterInvalid ? "filter-count-invalid" : ""}`}>
          {filterQuery.trim() === "" ? t('filter.noInput') :
            filterInvalid ? t('filter.invalidPattern') :
            filterSearching ? <><span className="filter-spinner" aria-hidden="true" /> {t('filter.results')}</> :
            `${formatCount(filterResults ? filterResults.length : 0)} ${t('filter.results')}`}
        </span>
        <button
          type="button"
          className={`filter-toggle ${filterMatchCase ? "active" : ""}`}
          title={t('filter.matchCase')}
          aria-pressed={filterMatchCase}
          onClick={() => setFilterMatchCase((v) => !v)}
        >Aa</button>
        <button
          type="button"
          className={`filter-toggle ${filterNegate ? "active" : ""}`}
          title={t('filter.negate')}
          aria-pressed={filterNegate}
          onClick={() => setFilterNegate((v) => !v)}
        ><Icon name="negate" /></button>
        <button
          type="button"
          className={`filter-toggle ${filterRegexMode ? "active" : ""}`}
          title={t('filter.regex')}
          aria-pressed={filterRegexMode}
          onClick={() => setFilterRegexMode((v) => !v)}
        >.*</button>
        <button type="button" className="filter-close" title={t('filter.close')} aria-label={t('filter.close')} onClick={() => setFilterOpen(false)}><Icon name="close" /></button>
      </div>}
      {!followTerminal && !tableView && completedWhilePaused && <button className="terminal-follow-button" title={t('btn.switchTableView')} onClick={() => { switchToTableMode(); setCompletedWhilePaused(false); }}><Icon name="grid" /></button>}
      {!followTerminal && !tableView && running && <button className="terminal-follow-button" title={t('btn.scrollBottom')} onClick={scrollTerminalToBottom}><Icon name="chevronDown" /></button>}
      {running && <button className="mobile-floating-stop" onClick={() => void solve()}>{t('btn.stopSolver')}</button>}
      {!showAll && totalPages > 1 && <div className={`page-switcher${pageSwitcherOpaque ? " page-switcher-opaque" : ""}`} ref={pageSwitcherRef}>
        <button className="page-switcher-btn page-switcher-prev" disabled={clampedPage === 0 || ((useLessRam || batchOffload) && isRestoring)} title={t('btn.prevPage')} onClick={(event) => { event.currentTarget.blur(); pageInputRef.current?.blur(); goToPage(clampedPage - 1, "bottom"); }}><Icon name="chevronLeft" /></button>
        <div className="page-switcher-center">
          <span className="page-switcher-inputwrap">
            <input
              ref={pageInputRef}
              className="page-switcher-input"
              type="text"
              inputMode="numeric"
              pattern="[0-9]*"
              name="sq1opt-page-index"
              id="sq1opt-page-index"
              autoComplete="one-time-code"
              autoCorrect="off"
              autoCapitalize="off"
              spellCheck={false}
              value={pageInput}
              aria-label={t('btn.pageNumber')}
              onBlur={() => { pageInputFocused.current = false; commitPageInput(); }}
              onChange={(event) => setPageInput(event.target.value.replace(/\D/g, "").slice(0, 6))}
              onKeyDown={(event) => { if (event.key === "Enter") commitPageInput(); }}
            />
            <span className="page-switcher-inputshadow" aria-hidden="true">{pageInput || "0"}</span>
          </span>
          <span className="page-switcher-total">/ {totalPages}</span>
        </div>
        <button className="page-switcher-btn page-switcher-next" disabled={clampedPage >= totalPages - 1 || ((useLessRam || batchOffload) && isRestoring)} title={t('btn.nextPage')} onClick={(event) => { event.currentTarget.blur(); pageInputRef.current?.blur(); goToPage(clampedPage + 1, "top"); }}><Icon name="chevronRight" /></button>
      </div>}
      {/* Intentional feature by Abid: table columns reflect the metric at solve time, not the live metric dropdown. */}
      {tableView && tableCols ? <div ref={tableContainerRef} className={`terminal metric-${tableMetricRef.current.toLowerCase()} ${showErgo ? "with-ergo" : ""}`} onScroll={handleTableScroll} onWheel={handleTableWheel} onTouchStart={handleTouchStart} onTouchMove={handleTouchMove} onTouchEnd={handleTouchEnd} onTouchCancel={() => { touchNavRef.current = null; }}>
        <div className="terminal-head" style={{ gridTemplateColumns: tableCols.template }}>{tableCols.hash && <span>{t('table.hash')}</span>}<b>{t('table.solution')}</b>{tableCols.angle && tableMetricRef.current === "ea" && <span>{t('table.angle')}</span>}{tableCols.move && tableMetricRef.current !== "es" && <span>{t('table.moves')}</span>}{tableCols.slices && <span>{t('table.slices')}</span>}{tableCols.ergo && showErgo && <span>{t('table.ergo')}</span>}</div>
        {tableSolutions.map((x, i) => {
          const ergo = displayErgo(x);
          const batchRow = !!x.rawPassThrough;
          const codeContent = batchRow ? x.alg : (outputMode === "abid" || (karn && abidNotation) ? abidify(x.alg) : (highlightActive ? highlightQuery(x.alg, filterAppliedQuery, filterMatchCase) : x.alg));
          return <div className="solution" style={{ gridTemplateColumns: tableCols.template }} key={x.raw} onMouseDown={(event) => {
            if (event.button !== 0 && event.button !== 2) return;
            event.preventDefault();
            if (event.button === 0) { if (contextMenu) setContextMenu(null); return; }
            setContextMenu({ x: event.clientX, y: event.clientY, alg: x.display });
          }} onContextMenu={(event) => event.preventDefault()}>{tableCols.hash && <span>{pageStart + i + 1}</span>}<code className={`${outputMode === "abid" || (karn && abidNotation) ? "abid" : ""} ${notationCleanClass}`} data-tooltip={x.debugAnn} onMouseEnter={positionTooltip}>{codeContent}</code>{tableCols.angle && tableMetricRef.current === "ea" && <span>{x.angle}</span>}{tableCols.move && tableMetricRef.current !== "es" && <span>{x.moves}</span>}{tableCols.slices && <span>{x.slices}</span>}{tableCols.ergo && showErgo && <span className={debugOutput && x.debugAnn ? "ergo-value clickable" : "ergo-value"} onClick={(event) => {
          if (!debugOutput || !x.debugAnn) return;
          event.stopPropagation();
          if (stickyTooltip?.key === x.raw) { setStickyTooltip(null); return; }
          const r = event.currentTarget.getBoundingClientRect();
          setStickyTooltip({ x: r.left + r.width / 2, y: r.top, text: x.debugAnn, key: x.raw });
        }}>{ergo === undefined ? "…" : ergo.toFixed(1)}</span>}</div>;
        })}
        {filterActive && !filterResults!.length && <div className="empty">{t('filter.noMatches')}</div>}
        {tableBusyMessage && <div className="table-busy"><span className="table-busy-spinner" /><span>{tableBusyText}</span></div>}
        {(useLessRam || batchOffload) && isRestoring && <div className="table-busy"><span className="table-busy-spinner" /><span>{t('terminal.loadingPage')}</span></div>}
      </div> : <div ref={terminalTextRef} className="terminal terminal-text" onWheel={handleTerminalWheel} onScroll={handleTerminalScroll} onMouseDown={markTerminalUserActive} onTouchStart={handleTouchStart} onTouchMove={handleTouchMove} onTouchEnd={handleTouchEnd} onTouchCancel={() => { touchNavRef.current = null; scheduleTerminalUserInactive(); }}>
        {(useLessRam || batchOffload) && isRestoring && <span className="terminal-line terminal-line-status">{t('terminal.loadingPage')}</span>}
        {!outputLines.length && !totalCount && <span className="terminal-line terminal-line-empty">{generator ? t('terminal.emptyScramble') : t('terminal.emptySolution')}</span>}
        {filterActive && !filterResults!.length && <span className="terminal-line terminal-line-empty">{t('filter.noMatches')}</span>}
        {terminalNonSolutions.map((line) => <span key={line.key} className={`terminal-line terminal-line-status ${notationCleanClass}`}>{line.text || " "}</span>)}
        {terminalSolutions.map((line, index) => <span key={line.key} className={`terminal-line terminal-line-solution ${index % 2 ? "terminal-line-b" : "terminal-line-a"} ${notationCleanClass}`}
          data-tooltip={line.solution.debugAnn}
          onMouseEnter={positionTooltip}
          onMouseDown={(event) => {
            if (event.button !== 0) return;
            event.preventDefault();
            if (contextMenu) setContextMenu(null);
          }}
          onContextMenu={(event) => { event.preventDefault(); setContextMenu({ x: event.clientX, y: event.clientY, alg: line.solution.display }); }}>{renderSolutionText(line.text)}</span>)}
        {statusLines.map((line, index) => <span key={`status-${index}-${line}`} className="terminal-line terminal-line-final">{line}</span>)}
      </div>}
      {researchMode && batchStatsText && <div className="batch-stats-panel">
        <div className="batch-stats-header">
          <span className="batch-stats-title">Stats</span>
          <button className="batch-stats-copy" title="Copy to clipboard" onClick={() => { navigator.clipboard.writeText(batchStatsText).catch(() => undefined); setStatusLines(["Stats copied to clipboard"]); }}>Copy</button>
        </div>
        <pre className="batch-stats-body">{batchStatsText}</pre>
      </div>}
    </div>
    );
  };
  return (
    <div ref={appRef} className={`app ${expanded ? "output-expanded" : ""} ${mobileOptionsOpen ? "mobile-options-open" : ""} ${mobileOutputOpen ? "mobile-output-open" : ""} ${stickyTooltip ? "sticky-tooltip-active" : ""}`} style={zoom === 1 ? undefined : { transform: `scale(${zoom})`, transformOrigin: "top left", width: `${100 / zoom}%`, height: `${100 / zoom}dvh` }}>
      <header>
        <img className="app-icon" src={`${import.meta.env.BASE_URL}icon-web.png`} alt="" />
        <div className="brand">
          <b>{t('app.brand')}</b><sub> &nbsp; &nbsp; {t('app.byline')}</sub>
        </div>
        <div className="top-menu-wrap">
          <button className="top-favorites-button" title={t('btn.favorites')} onClick={() => { setFavoritesOpen(true); setFavoritesOpening(true); }}><Icon name="heart" /></button>
          <button className="top-menu-button" aria-label={t('btn.openMenu')} aria-expanded={menu} title={tooltips.menu} onMouseDown={(event) => event.preventDefault()} onClick={() => setMenu((value) => !value)}>
            <Icon name="dots" />
          </button>
          {menu && (
            <div className="top-menu" onClick={(event) => event.stopPropagation()}>
              <button
                onClick={() => {
                  setModal("settings");
                  setMenu(false);
                }}
              >
                {t('btn.settings')}
              </button>
              <button
                onClick={() => {
                  setModal("how");
                  setMenu(false);
                }}
              >
                {t('btn.howToUse')}
              </button>
              <button
                onClick={() => {
                  setModal("about");
                  setMenu(false);
                }}
              >
                {t('btn.about')}
              </button>
            </div>
          )}
        </div>
      </header>
      {toast && <div className="toast" role="status">{toast}</div>}
      <div className="inputbar">
        <div className="mode-control" ref={modeControlRef}>
          <button className="mode" title={tooltips.inputMode} onMouseDown={(event) => event.preventDefault()} onClick={cycleMode}>
            {mode}
          </button>
          <button
            className="arrow"
            aria-label={t('tooltips.modeMenu')}
            title={tooltips.modeMenu}
            onMouseDown={(event) => event.preventDefault()}
            onClick={() => {
              setOpenDropdown(null);
              setModeMenu((v) => !v);
            }}
          >
            <Icon name="chevronDown" size={10} />
          </button>
          {modeMenu && (
            <div className="mode-menu">
              <button
                className={mode === "SCRAMBLE" ? "selected" : ""}
                onMouseDown={(event) => event.preventDefault()}
                onClick={() => chooseMode("SCRAMBLE")}
              >
                {t('input.scramble')}
              </button>
              <button
                className={mode === "ALG" ? "selected" : ""}
                onMouseDown={(event) => event.preventDefault()}
                onClick={() => chooseMode("ALG")}
              >
                {t('input.alg')}
              </button>
              <button
                className={mode === "POSITION" ? "selected" : ""}
                onMouseDown={(event) => event.preventDefault()}
                onClick={() => chooseMode("POSITION")}
              >
                {t('input.position')}
              </button>
            </div>
          )}
        </div>
        <div className="input-control">
          <input
            className={inputError ? "has-error" : ""}
            title={inputError}
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter" && !e.ctrlKey && !e.metaKey && !e.altKey) { e.preventDefault(); void apply(e.shiftKey); }
              if (e.key === "Escape") {
                e.preventDefault(); setUndo([]); setRedo([]); ignoreHistory.current = true;
                cubeActions.current?.reset(); ignoreHistory.current = false;
              }
            }}
            placeholder={
              mode === "POSITION"
                ? t('input.placeholderPosition')
                : t('input.placeholderMoves')
            }
          />
          <button className="apply" title={tooltips.apply} onClick={() => void apply()}>
            {t('btn.apply')}
          </button>
          {inputError && <span className="input-error">{inputError}</span>}
        </div>
      </div>
      <div className="main-grid" ref={mainGridRef}>
        {researchMode ? <aside className="cube-column research-batch-column" ref={cubeColumnRef}>
          <h2 className="batch-heading">Batch Input</h2>
          <label className="batch-target-label">Target position
            <textarea
              className="batch-target-input"
              value={batchTarget}
              onChange={(e) => setBatchTarget(e.target.value)}
              placeholder={"Leave empty for solved state\nOne target per line for multiple"}
              spellCheck={false}
              disabled={batchRunning}
              rows={2}
            />
          </label>
          <div
            className={`batch-drop-zone ${batchDragOver ? "drag-over" : ""}`}
            onDragOver={(e) => { e.preventDefault(); setBatchDragOver(true); }}
            onDragLeave={() => setBatchDragOver(false)}
            onDrop={(e) => {
              e.preventDefault();
              setBatchDragOver(false);
              const file = e.dataTransfer.files[0];
              if (file && file.name.endsWith(".txt")) {
                const reader = new FileReader();
                reader.onload = () => {
                  const text = typeof reader.result === "string" ? reader.result : "";
                  void appendBatchLines(text);
                };
                reader.readAsText(file);
              }
            }}
          >
            {batchStored ? (
              <div className="batch-stored-summary">
                <span className="batch-stored-text">Pasted {batchStoredLines} lines</span>
                <button
                  type="button"
                  className="batch-stored-clear"
                  title="Clear batch input"
                  disabled={batchRunning}
                  onClick={() => void clearBatchInput()}
                >Clear</button>
              </div>
            ) : (
              <textarea
                className="batch-textarea"
                value={batchInput}
                onChange={(e) => setBatchInput(e.target.value)}
                onPaste={(e) => {
                  e.preventDefault();
                  void appendBatchLines(e.clipboardData.getData("text"));
                }}
                placeholder={"Paste positions, one per line\nOr drag-drop a .txt file here"}
                spellCheck={false}
                disabled={batchRunning}
              />
            )}
          </div>
          <div className="batch-info">
            {batchStored ? `${batchStoredLines} position${batchStoredLines !== 1 ? "s" : ""}` : `${batchLineCount(batchInput)} position${batchLineCount(batchInput) !== 1 ? "s" : ""}`}
            {batchStored && <span className="batch-stored-badge">stored to disk</span>}
            {batchRunning && <span className="batch-running-badge">Running...</span>}
          </div>
          <div className="batch-actions">
            <button
              className={`solve ${batchRunning ? "is-running" : ""}`}
              disabled={batchStored ? !batchStoredLines : !batchInput.trim()}
              onClick={() => { if (batchRunning) { batchStopRef.current = true; const native = tauri(); native?.core?.invoke("stop_solver").catch(() => undefined); } else { void batchSolve(); } }}
            >{batchRunning ? "Stop" : "Batch Solve"}</button>
          </div>
          <div className="batch-export">
            <button disabled={!batchSolved} onClick={() => void downloadBatchCSV()}>Download CSV</button>
            <button disabled={!batchSolved} onClick={() => void generateBatchStats()}>Show Stats</button>
          </div>
        </aside> : <aside className="cube-column" ref={cubeColumnRef}>
          <Cube
            actionsRef={cubeActions}
            onChange={onCubeChange}
            onOptions={() => setMobileOptionsOpen(true)}
            shortcutsBlocked={Boolean(modal || favoritesOpen || openDropdown || modeMenu || menu || showAllConfirm || diskOpen || weightsOpen || contextMenu || stickyTooltip)}
          />
          <div className="moves">
            <button title={t('cube.titleUp')} onClick={() => cubeActions.current?.up()}>{t('btn.up')}</button>
            <button
              className="slice"
              title={t('cube.titleSlice')}
              onClick={() => cubeActions.current?.slice()}
            >
              {t('btn.slice')}
            </button>
            <button title={t('cube.titleU')} onClick={() => cubeActions.current?.u()}>{t('btn.u')}</button>
            <button title={t('cube.titleD')} onClick={() => cubeActions.current?.d()}>{t('btn.d')}</button>
            <button title={t('cube.titleDp')} onClick={() => cubeActions.current?.dp()}>{t('btn.dp')}</button>
          </div>
          <div className="undo">
            <button title={t('cube.titleUndo')} disabled={!undo.length || running} onClick={doUndo}>{t('btn.undo')}</button>
            <button title={t('cube.titleRedo')} disabled={!redo.length || running} onClick={doRedo}>{t('btn.redo')}</button>
          </div>
          <button className={`solve ${running ? "is-running" : ""}`} disabled={!running && twoGenBlocked} title={running ? t('cube.titleSolve') : twoGenBlocked ? t('cube.titleSolveBlocked') : commandPreview} onClick={() => void solve()}>{running ? t('btn.stop') : t('btn.solve')}</button>
          <button className="mobile-open-output" onClick={openMobileOutput}>{t('btn.openOutput')}</button>
        </aside>}
        {researchMode ? <div className="options-panel research-options" ref={optionsPanelRef}>
          <div className="mobile-modal-head">
            <b>Batch Options</b>
            <button aria-label={t('btn.closeOptions')} onClick={() => setMobileOptionsOpen(false)}><Icon name="close" /></button>
          </div>
          <h2>Batch Options</h2>
          <div className="select-grid">
            <OptionDropdown id="metric" label={t('options.metric')} title={tooltips.metric} value={metric} options={[{ value: "es", label: tList('options.metricValues')[0] }, { value: "move", label: tList('options.metricValues')[1] }, { value: "ea", label: tList('options.metricValues')[2] }]} disabled={batchRunning} open={openDropdown === "metric"} setOpen={setOpenDropdown} onChange={setMetric} />
            <OptionDropdown id="two" label={t('options.twoGen')} title={tooltips.twoGen} value={two} options={[{ value: "none", label: tList('options.twoGenValues')[0] }, { value: "p2g", label: tList('options.twoGenValues')[1] }, { value: "2g", label: tList('options.twoGenValues')[2] }]} disabled={batchRunning} highlight open={openDropdown === "two"} setOpen={setOpenDropdown} onChange={setTwo} />
            <OptionDropdown id="angle" label={t('options.lockLayer')} title={tooltips.angle} value={angle} options={[{ value: "none", label: tList('options.lockValues')[0] }, { value: "nb", label: tList('options.lockValues')[1] }, { value: "nu", label: tList('options.lockValues')[2] }, { value: "nd", label: tList('options.lockValues')[3] }]} disabled={batchRunning} highlight open={openDropdown === "angle"} setOpen={setOpenDropdown} onChange={setAngle} />
          </div>
          <div className="check-grid">
            <label title={tooltips.cubeshape}><input type="checkbox" checked={cubeShapeMemory} disabled={batchRunning} onChange={(e) => setCubeShapeMemory(e.target.checked)} /> {t('options.stayInCS')}</label>
          </div>
          <div className="limit-grid">
            <label title={tooltips.maxX}>{t('options.maxTop')}
              <div className="number-input-wrap">
                <input type="number" min="0" max="5" value={maxX ? maxXValue : ""} placeholder="6" disabled={batchRunning} onChange={(e) => updateOptionalLimit(e.target.value, 0, 5, setMaxX, setMaxXValue)} />
                <div className="number-stepper">
                  <button type="button" className="top-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxX, maxXValue, 1, 0, 5, setMaxX, setMaxXValue)}>▲</button>
                  <button type="button" className="bottom-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxX, maxXValue, -1, 0, 5, setMaxX, setMaxXValue)}>▼</button>
                </div>
              </div>
            </label>
            <label title={tooltips.maxY}>{t('options.maxBottom')}
              <div className="number-input-wrap">
                <input type="number" min="0" max="5" value={maxY ? maxYValue : ""} placeholder="6" disabled={batchRunning} onChange={(e) => updateOptionalLimit(e.target.value, 0, 5, setMaxY, setMaxYValue)} />
                <div className="number-stepper">
                  <button type="button" className="top-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxY, maxYValue, 1, 0, 5, setMaxY, setMaxYValue)}>▲</button>
                  <button type="button" className="bottom-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxY, maxYValue, -1, 0, 5, setMaxY, setMaxYValue)}>▼</button>
                </div>
              </div>
            </label>
            <label title={tooltips.maxTotal}>{t('options.maxTotal')}
              <div className="number-input-wrap">
                <input type="number" min="2" max="11" value={maxTotal ? maxTotalValue : ""} placeholder="12" disabled={batchRunning} onChange={(e) => updateOptionalLimit(e.target.value, 2, 11, setMaxTotal, setMaxTotalValue)} />
                <div className="number-stepper">
                  <button type="button" className="top-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxTotal, maxTotalValue, 1, 2, 11, setMaxTotal, setMaxTotalValue)}>▲</button>
                  <button type="button" className="bottom-stepper" disabled={batchRunning} onClick={() => stepOptionalLimit(maxTotal, maxTotalValue, -1, 2, 11, setMaxTotal, setMaxTotalValue)}>▼</button>
                </div>
              </div>
            </label>
          </div>
        </div> : renderOptionsPanel()}
        {renderOutputShell()}
      </div>
      {stickyTooltip && <div ref={stickyTooltipRef} className="sticky-tooltip" style={{ visibility: "hidden" }}>{stickyTooltip.text}</div>}
      {contextMenu && <div className="solution-context" style={{
        left: Math.max(0, Math.min(contextMenu.x, window.innerWidth - 180)),
        top: Math.max(0, Math.min(contextMenu.y, window.innerHeight - 80)),
      }} onClick={(event) => event.stopPropagation()} onContextMenu={(event) => event.preventDefault()}>
        <button onClick={() => { void navigator.clipboard.writeText(lineWithoutBracket(contextMenu.alg)); setContextMenu(null); }}>{t('btn.copyAlg')}</button>
        <button onClick={() => {
          const key = currentRunKey();
          setFavorites((old) => ({ ...old, [key]: {
            name: old[key]?.name || t('favorites.defaultName').replace('{{count}}', String(Object.keys(old).length + 1)),
            algorithms: Array.from(new Set([...(old[key]?.algorithms || []), contextMenu.alg])),
          } }));
          setContextMenu(null);
          setFavoritesOpen(true);
          setFavoritesOpening(true);
        }}>{t('btn.addFavorite')}</button>
      </div>}
      {createPortal(<>
      {modal && <Modal type={modal} close={() => history.back()} docNav={{
        openSq1optV2: () => navigateModal("sq1optv2"),
        openSq1optV1: () => navigateModal("sq1optv1"),
        openHowToUse: () => setModal("how"),
      }} settings={{
        ignoreTransforms, setIgnoreTransforms,
        debugOutput, setDebugOutput, zoom, setZoom, disabled: running, hasMaxTurn: maxX || maxY || maxTotal, language: lang,
        setLanguage: (code) => {
          setLang(code);
          setLangState(code);
          setToast(t('toast.languageSet'));
          if (toastTimerRef.current !== undefined) window.clearTimeout(toastTimerRef.current);
          toastTimerRef.current = window.setTimeout(() => setToast(null), 2600);
        },
        pageSize, setPageSize, showAll, setShowAll, pageSizeOptions: PAGE_SIZE_OPTIONS,
        useLessRam, setUseLessRam,
        onRequestShowAll: () => setShowAllConfirm(true),
        onOpenDiskSpace: () => setDiskOpen(true),
        onOpenWeights: () => setWeightsOpen(true),
      }} notation={{
        outputMode, setOutputMode, abidNotation, setAbidNotation, disabled: running,
      }} debugStats={modal === "debug" ? computeDebugStats() : null} liveDebug={modal === "debug" ? () => ({
        now: performance.now(),
        running: runningRef.current,
        startTime: solveStartTimeRef.current,
        stopTime: solveStopTimeRef.current,
        history: rateHistoryRef.current,
        totalSolutions: totalCountRefs(),
        totalNodes: progressNodesRef.current,
      }) : null} />}
      {modal === "settings" && diskOpen && <DiskSpaceModal onClose={() => setDiskOpen(false)} deleteOnQuit={deleteTablesOnQuit} setDeleteOnQuit={setDeleteTablesOnQuit} solutions={solutions} onClearSolutions={clearSolutions} />}
      {modal === "settings" && weightsOpen && <WeightsModal onClose={() => setWeightsOpen(false)} weightOverrides={weightOverrides} setWeightOverrides={setWeightOverrides} moveValueOverrides={moveValueOverrides} setMoveValueOverrides={setMoveValueOverrides} />}
      {showAllConfirm && <div className="modal-shade modal-shade-top" onClick={() => setShowAllConfirm(false)}>
        <div className="modal modal-confirm" onClick={(event) => event.stopPropagation()}>
          <button className="modal-close" onClick={() => setShowAllConfirm(false)}><Icon name="close" /></button>
          <h2>{t('modal.showAll.title')}</h2>
          <p>{t('modal.showAll.warning')}</p>
          <div className="modal-confirm-actions">
            <button onClick={() => setShowAllConfirm(false)}>{t('modal.showAll.cancel')}</button>
            <button className="modal-confirm-primary" onClick={() => { setShowAll(true); setShowAllConfirm(false); }}>{t('modal.showAll.confirm')}</button>
          </div>
        </div>
      </div>}
      {(favoritesOpen || favoritesClosing) && <div className={"modal-shade favorites-shade" + (favoritesClosing ? " closing" : "")} onPointerDown={(e) => { favShadeStartRef.current = e.target; }} onPointerUp={(e) => { favShadeEndRef.current = e.target; }} onClick={() => {
        const startOutside = !favShadeStartRef.current || !(favShadeStartRef.current as Element).closest(".favorites-modal");
        const endOutside = !favShadeEndRef.current || !(favShadeEndRef.current as Element).closest(".favorites-modal");
        if (startOutside && endOutside) beginCloseFavorites();
        favShadeStartRef.current = null;
        favShadeEndRef.current = null;
      }}>
        <div ref={favModalRef} className={"modal favorites-modal" + (favoritesClosing ? " closing" : "") + (favoritesOpening ? " opening" : "")} style={favoritesClosing ? { transformOrigin: `${favClosingOriginRef.current.x}% ${favClosingOriginRef.current.y}%` } : undefined} onAnimationEnd={favoritesClosing ? onFavCloseAnimEnd : favoritesOpening ? onFavOpenAnimEnd : undefined} onClick={(event) => event.stopPropagation()}>
          <button className="modal-close" onClick={beginCloseFavorites}><Icon name="close" /></button>
          <h2>{t('favorites.heading')}</h2>
          {!!totalCount && <button className="favorite-save" onClick={() => void (async () => {
            const key = currentRunKey();
            const algs = await collectAllDisplays();
            setFavorites((old) => ({ ...old, [key]: {
              name: old[key]?.name || t('favorites.defaultName').replace('{{count}}', String(Object.keys(old).length + 1)),
              algorithms: Array.from(new Set([...(old[key]?.algorithms || []), ...algs])),
            } }));
          })()}>{t('btn.saveSolutions')}</button>}
          {!Object.keys(favorites).length && <p>{t('favorites.empty')}</p>}
          {Object.entries(favorites).map(([key, bin]) => {
            const binDeleteKey = `bin::${key}`;
            const isBinPending = binDeleteKey in pendingDeletes;
            return <section className={`favorite-bin${isBinPending ? " favorite-bin-deleted" : ""}`} key={key}>
            <div className="favorite-bin-head">
              {isBinPending ? <><span>{t('favorites.binDeleted')}</span> <button className="favorite-undo" onClick={() => { clearTimeout(pendingDeletes[binDeleteKey]); setPendingDeletes((old) => { const next = { ...old }; delete next[binDeleteKey]; return next; }); }}>{t('favorites.undo')}</button>?</> : <>
              <input value={bin.name} aria-label={t('favorites.nameLabel')} onChange={(event) => setFavorites((old) => ({ ...old, [key]: { ...old[key], name: event.target.value } }))} />
              <div className="favorite-actions">
                <button title={t('btn.applySetup')} onClick={() => applyRunConfig(key)}>{t('btn.applySetup')}</button>
                <button title={t('btn.copyAllAlgs')} onClick={() => void navigator.clipboard.writeText(bin.algorithms.map(lineWithoutBracket).join("\n"))}><Icon name="copy" /></button>
                <button title={t('btn.deleteBin')} onClick={() => {
                  const timer = window.setTimeout(() => {
                    setPendingDeletes((old) => { const next = { ...old }; delete next[binDeleteKey]; return next; });
                    setFavorites((old) => { const next = { ...old }; delete next[key]; return next; });
                  }, 5000);
                  setPendingDeletes((old) => ({ ...old, [binDeleteKey]: timer }));
                }}><Icon name="trash" /></button>
              </div>
              </>}
            </div>
            {!isBinPending && <ul className="favorite-algs">
              {bin.algorithms.map((alg, i) => {
                const deleteKey = `${key}::${alg}`;
                const isPending = deleteKey in pendingDeletes;
                return <li key={`${alg}-${i}`} className={isPending ? "favorite-alg-deleted" : ""}>
                  {isPending ? <><span>{t('favorites.algDeleted')}</span> <button className="favorite-undo" onClick={() => { clearTimeout(pendingDeletes[deleteKey]); setPendingDeletes((old) => { const next = { ...old }; delete next[deleteKey]; return next; }); }}>{t('favorites.undo')}</button>?</> : <>
                    <code>{alg}</code>
                    <button className="favorite-alg-copy" title={t('btn.copyAlgSmall')} onClick={() => void navigator.clipboard.writeText(lineWithoutBracket(alg))}><Icon name="copy" size={12} /></button>
                    <button className="favorite-alg-remove" title={t('btn.remove')} onClick={() => {
                      const timer = window.setTimeout(() => {
                        setPendingDeletes((old) => { const next = { ...old }; delete next[deleteKey]; return next; });
                        setFavorites((old) => {
                          const bin = old[key];
                          if (!bin) return old;
                          const remaining = bin.algorithms.filter((_, j) => j !== i);
                          if (remaining.length === 0) { const next = { ...old }; delete next[key]; return next; }
                          return { ...old, [key]: { ...bin, algorithms: remaining } };
                        });
                      }, 5000);
                      setPendingDeletes((old) => ({ ...old, [deleteKey]: timer }));
                    }}><Icon name="close" size={12} /></button>
                  </>}
                </li>;
              })}
            </ul>}
          </section>})}
        </div>
      </div>}
      </>, document.body)}
    </div>
  );
}
