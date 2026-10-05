import React from "react";
import {
  AbsoluteFill, Audio, OffthreadVideo, Sequence, interpolate, spring,
  staticFile, useCurrentFrame, useVideoConfig, Easing,
} from "remotion";
import { loadFont as loadOrbitron } from "@remotion/google-fonts/Orbitron";
import { loadFont as loadGrotesk } from "@remotion/google-fonts/SpaceGrotesk";
import { loadFont as loadMono } from "@remotion/google-fonts/JetBrainsMono";
import { bf, beatAt, pulse, FPS } from "./beats";
import { COPY, LISTS, BARS } from "./copy";

const { fontFamily: DISPLAY } = loadOrbitron("normal", { weights: ["500", "700", "900"] });
const { fontFamily: BODY } = loadGrotesk("normal", { weights: ["400", "500", "700"] });
const { fontFamily: MONO } = loadMono("normal", { weights: ["400", "700"] });

// The device UI's own colours (firmware/main/ui.h), plus synthwave pink for contrast.
const SKY = "#38bdf8";
const OK = "#4ade80";
const AMBER = "#fbbf24";
const PINK = "#ff2e88";
const PURPLE = "#7c3aed";
const INK = "#05070d";
const PANEL = "#151a23";
const TRACK = "#2a3341";
const FG = "#e8edf2";
const DIM = "#a8b6c8";
const SEL = "#22406b";

// Scene boundaries, in beats (src/beats.ts). The 88% pick lands on the first drop (beat 6), the
// chatbot stalls in the bass dropout (34-37), and the logo takes the song's last phrase (86-102).
const S = {
  hook: 0, pick: 6, sort: 14, chat: 22, stall: 34, decide: 38, jev: 42, stats: 54,
  dentist: 62, pick2: 66, learn: 70, no: 78, logo: 86, end: 102,
};
const TAIL = 75;                                  // the song's reverb tail after its last beat
export const TOTAL_FRAMES = bf(S.end) + TAIL;

const clamp = { extrapolateLeft: "clamp", extrapolateRight: "clamp" } as const;
const outline = (px: number): React.CSSProperties =>
  ({ WebkitTextStroke: `${px}px ${INK}`, paintOrder: "stroke fill" });
const glow = (c: string, s = 1) =>
  `0 0 ${8 * s}px ${c}, 0 0 ${24 * s}px ${c}, 0 0 ${48 * s}px ${c}88`;

// Spring that starts at a beat.
const useIn = (beat: number, damping = 200, dur = 14) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  return spring({ frame: frame - bf(beat), fps, config: { damping, mass: 0.7 }, durationInFrames: dur });
};
const rnd = (i: number, n: number) => { const x = Math.sin(i * 127.1 + n * 311.7) * 43758.5453; return x - Math.floor(x); };

// ---------- background ----------

const Stars: React.FC = () => {
  const frame = useCurrentFrame();
  const stars = React.useMemo(() => Array.from({ length: 90 }, (_, i) =>
    ({ x: rnd(i, 1) * 1920, y: rnd(i, 2) * 520, s: 1 + rnd(i, 3) * 2.2, p: rnd(i, 4) * 6.28 })), []);
  return <>{stars.map((s, i) => (
    <div key={i} style={{
      position: "absolute", left: s.x, top: s.y, width: s.s, height: s.s, borderRadius: "50%",
      background: "white", opacity: 0.35 + 0.35 * Math.sin(frame / 12 + s.p),
    }} />
  ))}</>;
};

const HORIZON = 628;
const GridFloor: React.FC<{ phase: number }> = ({ phase }) => {
  const h = 1080 - HORIZON;
  const rows: number[] = [];
  for (let k = 1; k < 40; k++) {
    const z = k - phase;                       // depth; z=1 is the bottom edge
    if (z < 0.6) continue;
    rows.push(HORIZON + h / z);
  }
  const cols = Array.from({ length: 41 }, (_, i) => i - 20);
  const lines = (w: number, op: number) => (
    <g stroke={SKY} strokeWidth={w} opacity={op} fill="none">
      {rows.map((y, i) => <line key={`r${i}`} x1={0} x2={1920} y1={y} y2={y} strokeWidth={w * Math.min(1.6, 0.4 + (y - HORIZON) / h * 1.6)} />)}
      {cols.map((c) => <line key={`c${c}`} x1={960 + c * 26} y1={HORIZON} x2={960 + c * 230} y2={1080} />)}
    </g>
  );
  return (
    <svg width={1920} height={1080} viewBox="0 0 1920 1080" style={{ position: "absolute", inset: 0 }}>
      <defs>
        <linearGradient id="floorfade" x1="0" y1={HORIZON} x2="0" y2={1080} gradientUnits="userSpaceOnUse">
          <stop offset="0" stopColor={INK} stopOpacity={1} />
          <stop offset="0.35" stopColor={INK} stopOpacity={0.2} />
          <stop offset="1" stopColor={INK} stopOpacity={0} />
        </linearGradient>
        <filter id="gridglow" x="-5%" y="-5%" width="110%" height="110%"><feGaussianBlur stdDeviation="5" /></filter>
      </defs>
      <g filter="url(#gridglow)">{lines(6, 0.6)}</g>
      {lines(2, 0.9)}
      <rect x={0} y={HORIZON} width={1920} height={h} fill="url(#floorfade)" />
    </svg>
  );
};

const Synthwave: React.FC<{ sunRise: number; dim: number }> = ({ sunRise, dim }) => {
  const frame = useCurrentFrame();
  const b = Math.max(0, beatAt(frame));      // the grid moves one row per beat
  return (
    <AbsoluteFill style={{ background: `linear-gradient(#03050c 0%, #0a1733 45%, #123063 58%, ${INK} 58.2%)` }}>
      <Stars />
      <div style={{
        position: "absolute", left: 960 - 230, top: interpolate(sunRise, [0, 1], [700, 300]),
        width: 460, height: 460, borderRadius: "50%",
        background: `linear-gradient(${AMBER} 10%, #ff8a3d 45%, ${PINK} 72%, ${PURPLE} 100%)`,
        WebkitMaskImage: "linear-gradient(black 52%, transparent 52%, transparent 56%, black 56%, black 63%, transparent 63%, transparent 67%, black 67%, black 73%, transparent 73%, transparent 78%, black 78%, black 83%, transparent 83%, transparent 89%, black 89%)",
        filter: "drop-shadow(0 0 40px #ff2e8888)",
      }} />
      <svg width={1920} height={240} viewBox="0 0 1920 240" style={{ position: "absolute", left: 0, top: HORIZON - 240 }}>
        <polygon fill="#0c1a36" stroke={`${SKY}88`} strokeWidth={2}
          points="0,240 0,150 120,90 230,140 340,60 470,150 560,110 660,170 700,240 1220,240 1260,170 1350,100 1450,150 1560,50 1680,130 1780,80 1920,140 1920,240" />
        <polygon fill="#070f22" stroke={`${PINK}66`} strokeWidth={2}
          points="0,240 0,200 90,160 200,205 300,150 420,210 520,180 640,240 1280,240 1380,190 1480,215 1600,150 1720,200 1820,170 1920,195 1920,240" />
      </svg>
      <div style={{ position: "absolute", left: 0, right: 0, top: HORIZON, bottom: 0, background: INK }} />
      <GridFloor phase={b - Math.floor(b)} />
      <div style={{ position: "absolute", left: 0, right: 0, top: HORIZON - 4, height: 6, background: SKY, boxShadow: glow(SKY, 1.5) }} />
      <AbsoluteFill style={{ background: INK, opacity: dim }} />
    </AbsoluteFill>
  );
};

// ---------- shared bits ----------

const Title: React.FC<{ children: React.ReactNode; size?: number; color?: string; at: number; style?: React.CSSProperties }> =
  ({ children, size = 96, color = "white", at, style }) => {
    const p = useIn(at);
    return (
      <div style={{
        fontFamily: DISPLAY, fontWeight: 900, fontSize: size, color, letterSpacing: 2, textAlign: "center",
        textShadow: `0 0 14px ${INK}, 0 0 4px ${INK}, ${glow(color === "white" ? SKY : color, 0.8)}`,
        opacity: p, transform: `translateY(${(1 - p) * 18}px)`, filter: `blur(${(1 - p) * 6}px)`,
        ...outline(Math.max(7, size * 0.13)), ...style,
      }}>{children}</div>
    );
  };

const Sub: React.FC<{ children: React.ReactNode; at: number; style?: React.CSSProperties }> = ({ children, at, style }) => {
  const p = useIn(at);
  return <div style={{
    fontFamily: BODY, fontWeight: 500, fontSize: 40, color: "#dce8f5", textAlign: "center", ...outline(6),
    opacity: p, transform: `translateY(${(1 - p) * 10}px)`, ...style,
  }}>{children}</div>;
};

// ---------- the real device (hook and the Events pick) ----------

// public/clips/screen.mp4 is docs/media/demo.mp4's screen at 6x, real time, 30 fps. `src` is the
// clip frame shown at the shot's first frame, chosen so each pick lands on its beat: the 88% pick
// (src 346) on beat 6, the 68% pick (src 670) on beat 66. Each shot is uncut device time.
const SHOTS = {
  hook: { from: S.hook, to: S.sort, src: 346 - (bf(S.pick) - bf(S.hook)), pick: S.pick, row: [140, 274] },
  dentist: { from: S.dentist, to: S.learn, src: 670 - (bf(S.pick2) - bf(S.dentist)), pick: S.pick2, row: [536, 670] },
};
type Shot = typeof SHOTS.hook;
const SCR_W = 1440, SCR_H = 810;

const Screen: React.FC<{ scale: number; children: React.ReactNode; glowAmt?: number }> = ({ scale, children, glowAmt = 0 }) => (
  <div style={{
    padding: 20 * scale + 4, borderRadius: 30 * scale + 6, background: "#1a1f29",
    border: `3px solid ${SKY}`, boxShadow: `0 0 ${30 + 30 * glowAmt}px ${SKY}aa, 0 0 120px ${PURPLE}55`,
  }}>
    <div style={{ width: SCR_W * scale, height: SCR_H * scale, position: "relative", overflow: "hidden", borderRadius: 6, background: "black" }}>
      <div style={{ width: SCR_W, height: SCR_H, transform: `scale(${scale})`, transformOrigin: "0 0" }}>{children}</div>
      <div style={{ position: "absolute", inset: 0, background: "repeating-linear-gradient(transparent 0 3px, #00000018 3px 4px)" }} />
    </div>
  </div>
);

// Boxes around the pick, in clip pixels (6x the device's 240x135): the "AI 88%" header label and
// the selected row (device rows 24-44 for Shopping, 90-110 for Events).
const Highlight: React.FC<{ at: number; row: number[] }> = ({ at, row }) => {
  const frame = useCurrentFrame();
  if (frame < bf(at) || frame >= bf(at + 3.75)) return null;
  const p = spring({ frame: frame - bf(at), fps: FPS, config: { damping: 12, mass: 0.5 }, durationInFrames: 12 });
  const out = interpolate(frame, [bf(at + 3), bf(at + 3.75)], [1, 0], clamp);
  const box = (x0: number, y0: number, x1: number, y1: number): React.CSSProperties => ({
    position: "absolute", left: x0, top: y0, width: x1 - x0, height: y1 - y0, boxSizing: "border-box",
    border: `8px solid ${OK}`, borderRadius: 16, boxShadow: `${glow(OK, 1)}, inset 0 0 30px ${OK}66`,
    opacity: out * Math.min(1, p * 1.4), transform: `scale(${1.25 - 0.25 * p})`,
  });
  return <>
    <div style={box(1100, 12, 1428, 100)} />
    <div style={box(4, row[0], 1436, row[1])} />
  </>;
};

const Device: React.FC<{ shot: Shot; top: React.ReactNode; under?: React.ReactNode }> = ({ shot, top, under }) => {
  const frame = useCurrentFrame();
  const beat = pulse(frame, 4);
  const bump = frame >= bf(shot.pick) ? 0.05 * Math.exp(-(frame - bf(shot.pick)) / 5) : 0;
  return (
    <AbsoluteFill>
      <div style={{ position: "absolute", top: 22, left: 0, right: 0, display: "flex", justifyContent: "center" }}>{top}</div>
      <div style={{ position: "absolute", top: 128, left: 0, right: 0, display: "flex", justifyContent: "center", transform: `scale(${1 + bump})` }}>
        <Screen scale={0.86} glowAmt={beat}>
          <Sequence from={bf(shot.from)} durationInFrames={bf(shot.to) - bf(shot.from)} layout="none">
            <OffthreadVideo src={staticFile("clips/screen.mp4")} trimBefore={shot.src} muted style={{ width: SCR_W, height: SCR_H, display: "block" }} />
          </Sequence>
          <Highlight at={shot.pick} row={shot.row} />
        </Screen>
      </div>
      {under && <div style={{ position: "absolute", top: 892, left: 0, right: 0, display: "flex", justifyContent: "center" }}>{under}</div>}
      <div style={{ position: "absolute", bottom: 26, right: 150, fontFamily: BODY, fontWeight: 700, fontSize: 24, color: INK, background: AMBER, borderRadius: 8, padding: "6px 14px", letterSpacing: 1 }}>
        ● {COPY.demo.badge}
      </div>
      <div style={{ position: "absolute", bottom: 30, left: 150, fontFamily: BODY, fontSize: 24, color: "#cfdcec", ...outline(4) }}>
        {COPY.demo.caption}
      </div>
    </AbsoluteFill>
  );
};

// 1. Hook: the question while the chip thinks, the answer on the drop.
const Hook: React.FC = () => {
  const frame = useCurrentFrame();
  const H = COPY.hook;
  const answered = frame >= bf(S.pick);
  return (
    <Device shot={SHOTS.hook}
      top={answered
        ? <Title at={S.pick} size={68}><span style={{ color: AMBER }}>{H.yes}</span> {H.a}</Title>
        : <Title at={-0.6} size={68}>{H.q}</Title>}
      under={<Sub at={S.pick + 1.5} style={{ fontSize: 44, color: "white" }}>{H.sub}</Sub>} />
  );
};

// 7. A second note: a time in it, and the pick is Events.
const Dentist: React.FC = () => {
  const frame = useCurrentFrame();
  const b = beatAt(frame);
  const steps = COPY.demo.steps;
  const step = [...steps].reverse().find((s) => b >= s.at) ?? steps[0];
  const lp = spring({ frame: frame - bf(step.at), fps: FPS, config: { damping: 200 }, durationInFrames: 10 });
  return (
    <Device shot={SHOTS.dentist} top={
      <div style={{ display: "flex", alignItems: "center", gap: 22, opacity: lp, transform: `translateY(${(1 - lp) * -16}px)`, marginTop: 14 }}>
        <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 44, color: INK, background: SKY, borderRadius: 10, padding: "4px 16px", boxShadow: glow(SKY, 0.6) }}>{step.n}</div>
        <div style={{ fontFamily: DISPLAY, fontWeight: 700, fontSize: 52, color: "white", textShadow: glow(SKY, 0.6), ...outline(7) }}>{step.label}</div>
      </div>
    } />
  );
};

// ---------- 2. sort: note cards fly into their lists ----------

const COL_W = 285, COL_GAP = 18, COL_X0 = (1920 - (6 * COL_W + 5 * COL_GAP)) / 2;
const CARD_H = 74, HDR_Y = 290, CARD_Y0 = 372, CARD_STEP = 86;

type Card = { text: string; col: number; row: number; order: number };
const CARDS: Card[] = (() => {
  const flat: Card[] = [];
  LISTS.forEach((l, col) => l.notes.forEach((text, row) => flat.push({ text, col, row, order: 0 })));
  // filing order: shuffled, so the lists fill in a scatter
  flat.map((c, i) => ({ c, k: rnd(i, 9) })).sort((a, b) => a.k - b.k).forEach((x, i) => { x.c.order = i; });
  return flat;
})();

// Each card starts off screen on a random edge.
const startPos = (c: Card) => {
  const a = rnd(c.order, 5) * Math.PI * 2;
  return { x: 960 - COL_W / 2 + Math.cos(a) * 1400, y: 540 + Math.sin(a) * 900, r: (rnd(c.order, 7) - 0.5) * 60 };
};
const slotPos = (c: Card) => ({ x: COL_X0 + c.col * (COL_W + COL_GAP), y: CARD_Y0 + c.row * CARD_STEP });
const fileAt = (c: Card) => S.sort + 0.25 + c.order * 0.25;        // one card every quarter beat
const LAND = 8;                                                      // frames from launch to landing

const NoteCards: React.FC = () => {
  const frame = useCurrentFrame();
  return <>{CARDS.map((c, i) => {
    if (frame < bf(fileAt(c))) return null;
    const p0 = startPos(c), p1 = slotPos(c);
    const f = spring({ frame: frame - bf(fileAt(c)), fps: FPS, config: { damping: 200 }, durationInFrames: LAND + 1 });
    const landedAt = bf(fileAt(c)) + LAND;
    const landed = frame >= landedAt;
    const hit = landed ? interpolate(frame, [landedAt, landedAt + 12], [1, 0], clamp) : 0;
    return (
      <div key={i} style={{
        position: "absolute", left: p0.x + (p1.x - p0.x) * f, top: p0.y + (p1.y - p0.y) * f, width: COL_W, height: CARD_H, boxSizing: "border-box",
        transform: `rotate(${p0.r * (1 - f)}deg) scale(${1 + 0.15 * Math.sin(Math.PI * f)})`,
        background: PANEL, border: `2px solid ${landed ? SKY : TRACK}`, borderRadius: 10, padding: "10px 14px",
        fontFamily: MONO, fontSize: 21, lineHeight: 1.25, color: FG,
        boxShadow: `0 10px 30px #000a, 0 0 ${28 * hit}px ${SKY}`,
        display: "flex", alignItems: "center",
      }}>{c.text}</div>
    );
  })}</>;
};

const ListHeaders: React.FC = () => {
  const frame = useCurrentFrame();
  return <>{LISTS.map((l, col) => {
    const p = spring({ frame: frame - bf(S.sort + col * 0.15), fps: FPS, config: { damping: 200 }, durationInFrames: 8 });
    const n = CARDS.filter((c) => c.col === col && frame >= bf(fileAt(c)) + LAND).length;
    return (
      <div key={col} style={{
        position: "absolute", left: COL_X0 + col * (COL_W + COL_GAP), top: HDR_Y, width: COL_W, height: 62,
        boxSizing: "border-box", borderRadius: 10, background: SEL, border: `2px solid ${SKY}`,
        display: "flex", alignItems: "center", justifyContent: "space-between", padding: "0 18px",
        opacity: p, transform: `translateY(${(1 - p) * -30}px)`, boxShadow: glow(SKY, 0.35),
      }}>
        <span style={{ fontFamily: DISPLAY, fontWeight: 700, fontSize: 26, color: "white" }}>{l.name}</span>
        <span style={{ fontFamily: MONO, fontWeight: 700, fontSize: 24, color: SKY }}>{n}</span>
      </div>
    );
  })}</>;
};

const Sort: React.FC = () => (
  <AbsoluteFill>
    <ListHeaders />
    <NoteCards />
    <AbsoluteFill style={{ alignItems: "center", paddingTop: 90 }}>
      <Title at={S.sort} size={84}>{COPY.sort.title}</Title>
    </AbsoluteFill>
  </AbsoluteFill>
);

// ---------- 3. chatbots write word by word ----------

const ChatScene: React.FC = () => {
  const frame = useCurrentFrame();
  const words = COPY.chat.reply.split(" ");
  const b = Math.min(beatAt(frame), S.stall);      // the bass drops out: generation hangs
  const n = Math.max(0, Math.min(words.length, Math.floor((b - (S.chat + 1.5)) * 2) + 1));   // a word per half beat
  const stalled = beatAt(frame) >= S.stall;
  const ask = useIn(S.chat + 0.5);
  const bot = useIn(S.chat + 1);
  const push = interpolate(frame, [bf(S.stall), bf(S.decide)], [1, 1.06], { ...clamp, easing: Easing.in(Easing.cubic) });
  return (
    <AbsoluteFill style={{ alignItems: "center", paddingTop: 70 }}>
      <Title at={S.chat} size={76}>{COPY.chat.title}</Title>
      <div style={{ marginTop: 70, width: 1340, display: "flex", flexDirection: "column", gap: 26, transform: `scale(${push})` }}>
        <div style={{
          alignSelf: "flex-end", maxWidth: 900, padding: "20px 28px", borderRadius: "26px 26px 6px 26px",
          background: SKY, color: INK, fontFamily: BODY, fontWeight: 500, fontSize: 34,
          opacity: ask, transform: `translateY(${(1 - ask) * 20}px)`,
        }}>{COPY.chat.ask}</div>
        <div style={{
          alignSelf: "flex-start", width: 1100, minHeight: 200, padding: "22px 30px", borderRadius: "26px 26px 26px 6px",
          background: PANEL, border: `2px solid ${TRACK}`, color: FG, fontFamily: BODY, fontSize: 36, lineHeight: 1.4,
          opacity: bot, boxSizing: "border-box",
        }}>
          {words.slice(0, n).join(" ")}
          <span style={{ color: SKY, opacity: frame % 14 < 7 ? 1 : 0.15 }}> ▋</span>
          <div style={{ marginTop: 16, fontFamily: MONO, fontSize: 24, color: stalled ? AMBER : DIM }}>
            {COPY.chat.generating}{stalled ? ".".repeat(1 + (Math.floor(frame / 8) % 3)) : ""} · {n} {COPY.chat.tokens}
          </div>
        </div>
        <div style={{ alignSelf: "flex-start", fontFamily: BODY, fontSize: 24, color: "#8a97a8", opacity: bot, ...outline(4) }}>{COPY.chat.caption}</div>
      </div>
    </AbsoluteFill>
  );
};

// ---------- 4. this one decides ----------

const Decide: React.FC = () => {
  const frame = useCurrentFrame();
  const punch = bf(S.decide);
  const pp = spring({ frame: frame - punch, fps: FPS, config: { damping: 200 }, durationInFrames: 8 });
  const split = 14 * Math.exp(-(frame - punch) / 3);
  return (
    <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
      <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 96, color: "white", letterSpacing: 6, ...outline(10), textShadow: glow(SKY, 0.6), opacity: pp }}>
        {COPY.decide.a}
      </div>
      <div style={{ position: "relative", transform: `scale(${1.2 - 0.2 * pp})` }}>
        {[[PINK, -split], [SKY, split], ["white", 0]].map(([c, dx], i) => (
          <div key={i} style={{
            position: i < 2 ? "absolute" : "relative", left: 0, top: 0, width: "100%",
            fontFamily: DISPLAY, fontWeight: 900, fontSize: 230, color: c as string, textAlign: "center",
            transform: `translateX(${dx}px)`, mixBlendMode: i < 2 ? "screen" : "normal",
            textShadow: i === 2 ? glow(AMBER, 1.1) : "none", opacity: i < 2 ? 0.8 : 1, whiteSpace: "nowrap",
            ...(i === 2 ? outline(16) : {}),
          }}>{COPY.decide.b}</div>
        ))}
      </div>
    </AbsoluteFill>
  );
};

// ---------- 5. Jev-style: one pass, every list scored at once ----------

const JevScene: React.FC = () => {
  const frame = useCurrentFrame();
  const left = useIn(S.jev + 0.5);
  const beamAt = S.jev + 2;
  const beam = interpolate(frame, [bf(beamAt), bf(beamAt + 0.5)], [0, 1], { ...clamp, easing: Easing.in(Easing.quad) });
  const fill = spring({ frame: frame - bf(beamAt + 0.5), fps: FPS, config: { damping: 200 }, durationInFrames: 10 });
  const hit = frame >= bf(beamAt + 0.5) ? interpolate(frame, [bf(beamAt + 0.5), bf(beamAt + 0.5) + 14], [1, 0], clamp) : 0;
  const beat = pulse(frame, 4);
  return (
    <AbsoluteFill style={{ alignItems: "center", paddingTop: 70 }}>
      <Title at={S.jev} size={72}>{COPY.jev.title}</Title>
      <div style={{ position: "absolute", top: 330, left: 120, width: 1680, height: 420, display: "flex", alignItems: "center" }}>
        <div style={{
          width: 520, padding: "28px 30px", borderRadius: 18, background: PANEL, border: `3px solid ${SKY}`, boxSizing: "border-box",
          fontFamily: MONO, fontSize: 36, lineHeight: 1.3, color: FG, opacity: left, transform: `translateX(${(1 - left) * -40}px)`,
          boxShadow: glow(SKY, 0.4),
        }}>
          <div style={{ fontFamily: BODY, fontSize: 22, color: SKY, letterSpacing: 3, marginBottom: 10 }}>{COPY.jev.noteLabel}</div>
          {COPY.jev.note}
        </div>
        <div style={{ position: "relative", width: 380, height: 120, opacity: left }}>
          <div style={{ position: "absolute", left: 30, right: 30, top: 58, height: 4, background: TRACK, borderRadius: 2 }} />
          <div style={{ position: "absolute", left: 30, top: 58, height: 4, width: beam * 320, background: AMBER, boxShadow: glow(AMBER, 0.6) }} />
          <div style={{
            position: "absolute", left: 30, top: 50, height: 20, width: 20, borderRadius: 10, background: "white",
            transform: `translateX(${beam * 300}px)`, boxShadow: glow(AMBER, 1.2), opacity: beam > 0 && beam < 1 ? 1 : 0,
          }} />
          <div style={{
            position: "absolute", left: "50%", top: 0, transform: "translateX(-50%)", fontFamily: DISPLAY, fontWeight: 900, fontSize: 30,
            color: INK, background: AMBER, borderRadius: 8, padding: "4px 14px", boxShadow: glow(AMBER, 0.4 + 0.5 * beat),
          }}>{COPY.jev.pass}</div>
        </div>
        <div style={{
          width: 680, padding: "26px 30px", borderRadius: 18, background: "#0b0e14ee", border: `3px solid ${OK}`, boxSizing: "border-box",
          display: "flex", flexDirection: "column", gap: 18, opacity: left, boxShadow: glow(OK, 0.3 + 0.9 * hit),
        }}>
          {BARS.map((b) => {
            const top = b.p > 50;
            return (
              <div key={b.name} style={{ display: "flex", alignItems: "center", gap: 20 }}>
                <div style={{ width: 170, fontFamily: BODY, fontWeight: 700, fontSize: 32, color: "white" }}>{b.name}</div>
                <div style={{ flex: 1, height: 30, background: TRACK, borderRadius: 6, overflow: "hidden" }}>
                  <div style={{ width: `${Math.max(b.p, 1) * fill}%`, height: "100%", background: top ? OK : SKY, boxShadow: glow(top ? OK : SKY, 0.4) }} />
                </div>
                <div style={{ width: 92, textAlign: "right", fontFamily: MONO, fontWeight: 700, fontSize: 32, color: top ? OK : DIM }}>
                  {Math.round(b.p * fill)}%
                </div>
              </div>
            );
          })}
        </div>
      </div>
      <Sub at={S.jev + 4} style={{ position: "absolute", top: 820, fontSize: 48, color: "white" }}>{COPY.jev.sub}</Sub>
      <Sub at={S.jev + 6} style={{ position: "absolute", top: 900, fontSize: 30, fontFamily: MONO, color: DIM }}>{COPY.jev.foot}</Sub>
    </AbsoluteFill>
  );
};

// ---------- 6. numbers ----------

const Stat: React.FC<{ at: number; big: string; small: string; color: string }> = ({ at, big, small, color }) => {
  const frame = useCurrentFrame();
  const p = useIn(at);
  const beat = frame >= bf(at) ? pulse(frame, 4) : 0;
  return (
    <div style={{
      width: 480, height: 300, borderRadius: 26, border: `4px solid ${color}`, boxSizing: "border-box",
      background: "#0b0e14d0", boxShadow: `${glow(color, 0.4 + 0.5 * beat)}, inset 0 0 60px ${color}33`,
      display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center",
      opacity: p, transform: `scale(${0.9 + 0.1 * p})`,
    }}>
      <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 104, color: "white", textShadow: glow(color, 0.9), lineHeight: 1, whiteSpace: "nowrap", ...outline(10) }}>{big}</div>
      <div style={{ fontFamily: BODY, fontWeight: 700, fontSize: 40, color, marginTop: 20, textTransform: "uppercase", letterSpacing: 4, ...outline(6) }}>{small}</div>
    </div>
  );
};

const Stats: React.FC = () => {
  const frame = useCurrentFrame();
  const C = COPY.stats;
  const barIn = useIn(S.stats + 3);
  const squashAt = S.stats + 4.5;
  const squash = spring({ frame: frame - bf(squashAt), fps: FPS, config: { damping: 13, mass: 0.6 }, durationInFrames: 20 });
  const FULL = 1500, SMALL = FULL * 6.2 / 41.6;          // bar widths to scale: 41.6 MB vs 6.2 MB
  const w = FULL + (SMALL - FULL) * squash;
  const squashed = frame >= bf(squashAt);
  return (
    <AbsoluteFill style={{ alignItems: "center" }}>
      <div style={{ display: "flex", gap: 50, marginTop: 110 }}>
        {C.cards.map((c, i) => <Stat key={i} at={S.stats + i} big={c.big} small={c.small} color={[PINK, SKY, AMBER][i]} />)}
      </div>
      <div style={{ marginTop: 90, width: FULL, height: 150, position: "relative", opacity: barIn }}>
        <div style={{
          position: "absolute", left: (FULL - w) / 2, width: w, height: 130, top: 0, borderRadius: 14,
          background: squashed ? `linear-gradient(90deg, ${SKY}, ${OK})` : `repeating-linear-gradient(90deg, ${PINK} 0 6px, #c21f66 6px 12px)`,
          boxShadow: glow(squashed ? SKY : PINK, 0.6), display: "flex", alignItems: "center", justifyContent: "center",
        }}>
          <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 64, color: "white", whiteSpace: "nowrap", ...outline(10) }}>
            {squashed ? C.to : C.from}
          </div>
        </div>
        <div style={{
          position: "absolute", top: 146, left: 0, right: 0, textAlign: "center", fontFamily: BODY, fontWeight: 700, fontSize: 34,
          color: squashed ? SKY : PINK, letterSpacing: 3, textTransform: "uppercase", ...outline(5),
        }}>{squashed ? C.toLabel : C.fromLabel}</div>
      </div>
      <Sub at={S.stats + 5.5} style={{ position: "absolute", top: 930, fontSize: 44, color: "white" }}>{C.sub}</Sub>
    </AbsoluteFill>
  );
};

// ---------- 8. it learns ----------

const Learn: React.FC = () => {
  const frame = useCurrentFrame();
  const L = COPY.learn;
  const a = useIn(S.learn + 1);
  const bIn = useIn(S.learn + 2);
  const count = interpolate(frame, [bf(S.learn + 2), bf(S.learn + 3.5)], [L.from, L.to], { ...clamp, easing: Easing.out(Easing.cubic) });
  const doneAt = bf(S.learn + 3.5);
  const hit = frame >= doneAt ? interpolate(frame, [doneAt, doneAt + 12], [1, 0], clamp) : 0;
  const num = (c: string, s: number): React.CSSProperties => ({
    fontFamily: DISPLAY, fontWeight: 900, fontSize: 190 * s, color: c, lineHeight: 1, textShadow: glow(c, 0.8), ...outline(14),
  });
  return (
    <AbsoluteFill style={{ alignItems: "center", paddingTop: 70 }}>
      <Title at={S.learn} size={96}>{L.title}</Title>
      <div style={{ display: "flex", alignItems: "center", gap: 70, marginTop: 90 }}>
        <div style={{ textAlign: "center", opacity: a }}>
          <div style={num(DIM, 0.75)}>{L.from}%</div>
          <div style={{ fontFamily: BODY, fontWeight: 700, fontSize: 34, color: DIM, marginTop: 18, ...outline(5) }}>{L.fromLabel}</div>
        </div>
        <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 110, color: AMBER, opacity: bIn, textShadow: glow(AMBER, 0.6) }}>→</div>
        <div style={{ textAlign: "center", opacity: bIn, transform: `scale(${1 + 0.12 * hit})` }}>
          <div style={num(OK, 1)}>{Math.round(count)}%</div>
          <div style={{ fontFamily: BODY, fontWeight: 700, fontSize: 34, color: OK, marginTop: 18, ...outline(5) }}>{L.toLabel}</div>
        </div>
      </div>
      <Sub at={S.learn + 1} style={{ marginTop: 30, fontSize: 32, fontFamily: MONO, color: DIM, letterSpacing: 2, textTransform: "uppercase" }}>{L.unit}</Sub>
      <Sub at={S.learn + 4} style={{ position: "absolute", top: 900, fontSize: 48, color: "white" }}>{L.sub}</Sub>
    </AbsoluteFill>
  );
};

// ---------- 9. offline ----------

const NoScene: React.FC = () => (
  <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", gap: 20 }}>
    {COPY.no.map((t, i) => <Title key={i} at={S.no + i * 1.5} size={92} color={[SKY, AMBER][i]}>{t}</Title>)}
  </AbsoluteFill>
);

// ---------- 10. logo (the song's last phrase and its ending) ----------

const NoteIcon: React.FC<{ size: number; spark: number }> = ({ size, spark }) => (
  <svg width={size} height={size} viewBox="0 0 100 100" style={{ overflow: "visible" }}>
    <rect x={6} y={14} width={80} height={76} rx={12} fill="#10151f" stroke={SKY} strokeWidth={4} />
    <rect x={6} y={14} width={80} height={16} rx={8} fill={SKY} />
    <rect x={18} y={42} width={50} height={7} rx={3} fill={FG} />
    <rect x={18} y={56} width={38} height={7} rx={3} fill={SEL} stroke={SKY} strokeWidth={1.5} />
    <rect x={18} y={70} width={44} height={7} rx={3} fill={DIM} />
    <g transform={`translate(84 18) scale(${0.8 + 0.4 * spark})`}>
      <path d="M0,-16 C2,-4 4,-2 16,0 C4,2 2,4 0,16 C-2,4 -4,2 -16,0 C-4,-2 -2,-4 0,-16 Z" fill={AMBER} style={{ filter: `drop-shadow(0 0 6px ${AMBER})` }} />
    </g>
  </svg>
);

const Logo: React.FC = () => {
  const frame = useCurrentFrame();
  const L = COPY.logo;
  const beat = frame < bf(S.end) ? pulse(frame, 4) : 0;
  const lp = useIn(S.logo, 14, 16);
  const urlIn = useIn(S.logo + 8);
  return (
    <AbsoluteFill style={{ alignItems: "center", paddingTop: 110 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 46, opacity: Math.min(1, lp * 1.5), transform: `scale(${1.25 - 0.25 * lp})` }}>
        <div style={{ filter: `drop-shadow(0 0 ${10 + 30 * beat}px ${SKY})` }}><NoteIcon size={200} spark={beat} /></div>
        <div style={{ fontFamily: DISPLAY, fontWeight: 900, fontSize: 132, color: "white", textShadow: glow(SKY, 0.8 + 0.6 * beat), whiteSpace: "nowrap", ...outline(12) }}>
          {L.a} <span style={{ color: SKY }}>{L.b}</span>
        </div>
      </div>
      <Title at={S.logo + 2} size={66} color={AMBER} style={{ marginTop: 34 }}>{L.tagline}</Title>
      <div style={{ display: "flex", gap: 22, marginTop: 44 }}>
        {L.chips.map((c, i) => {
          const p = spring({ frame: frame - bf(S.logo + 4 + i), fps: FPS, config: { damping: 200 }, durationInFrames: 10 });
          return <div key={i} style={{
            fontFamily: BODY, fontWeight: 700, fontSize: 32, color: "white", padding: "10px 24px", borderRadius: 40,
            border: `2px solid ${SKY}`, background: "#0b0e14cc", opacity: p, transform: `translateY(${(1 - p) * 16}px)`,
          }}>{c}</div>;
        })}
      </div>
      <div style={{
        marginTop: 56, fontFamily: MONO, fontWeight: 700, fontSize: 48, color: INK, background: SKY, borderRadius: 14, padding: "14px 34px",
        boxShadow: glow(SKY, 0.5 + 0.5 * beat), opacity: urlIn, transform: `scale(${0.92 + 0.08 * urlIn})`,
      }}>{L.url}</div>
      <Sub at={S.logo + 9.5} style={{ marginTop: 22, fontSize: 32, color: DIM }}>{L.foot}</Sub>
    </AbsoluteFill>
  );
};

// ---------- composition ----------

export const Promo: React.FC = () => {
  const frame = useCurrentFrame();
  const b = beatAt(frame);
  // the sun rises on the drop, sets behind "It learns from you" and comes back up for the logo
  const sunRise = interpolate(frame, [bf(S.pick), bf(S.pick + 4), bf(S.learn), bf(S.learn + 3), bf(S.logo), bf(S.logo + 6)], [0, 1, 1, 0, 0, 0.5],
    { ...clamp, easing: Easing.inOut(Easing.cubic) });
  // background dim per scene: the cards and the screen need calm behind them
  const dimFor = (x: number) => x < S.sort ? 0.55 : x < S.chat ? 0.45 : x < S.decide ? 0.4 : x < S.jev ? 0.1
    : x < S.stats ? 0.3 : x < S.dentist ? 0.25 : x < S.learn ? 0.55 : x < S.no ? 0.55 : x < S.logo ? 0.4 : 0.05;
  const dim = dimFor(b) * 0.7 + dimFor(b - 0.5) * 0.3;
  const hard = [S.pick, S.decide, S.learn, S.logo];
  const soft = [S.sort, S.chat, S.jev, S.stats, S.dentist, S.pick2, S.no];
  const flash = Math.max(0,
    ...hard.map((x) => frame >= bf(x) ? interpolate(frame, [bf(x), bf(x) + 9], [0.6, 0], clamp) : 0),
    ...soft.map((x) => frame >= bf(x) ? interpolate(frame, [bf(x), bf(x) + 6], [0.22, 0], clamp) : 0),
  );
  // the song's last beat is beat 102; fade to black over its reverb tail
  const fadeOut = interpolate(frame, [bf(S.end) + 15, TOTAL_FRAMES], [1, 0], clamp);
  return (
    <AbsoluteFill style={{ background: "black" }}>
      <Audio src={staticFile("music.wav")} />
      <AbsoluteFill style={{ opacity: fadeOut }}>
        <Synthwave sunRise={sunRise} dim={dim} />
        {/* scenes read the absolute frame (their timings are in song beats) */}
        {frame < bf(S.sort) && <Hook />}
        {frame >= bf(S.sort) && frame < bf(S.chat) && <Sort />}
        {frame >= bf(S.chat) && frame < bf(S.decide) && <ChatScene />}
        {frame >= bf(S.decide) && frame < bf(S.jev) && <Decide />}
        {frame >= bf(S.jev) && frame < bf(S.stats) && <JevScene />}
        {frame >= bf(S.stats) && frame < bf(S.dentist) && <Stats />}
        {frame >= bf(S.dentist) && frame < bf(S.learn) && <Dentist />}
        {frame >= bf(S.learn) && frame < bf(S.no) && <Learn />}
        {frame >= bf(S.no) && frame < bf(S.logo) && <NoScene />}
        {frame >= bf(S.logo) && <Logo />}
      </AbsoluteFill>
      <AbsoluteFill style={{ background: "white", opacity: flash, pointerEvents: "none" }} />
    </AbsoluteFill>
  );
};
