// Time formatting of the views (Section 4.4.1): local time HH:MM and HH:MM:SS, distance −MM:SS.
// No DOM access, so the formatting is also used by the unit tests.

const MINUS = '−';

function pad2(n) {
  return String(n).padStart(2, '0');
}

// UNIX seconds → local "HH:MM".
export function formatHHMM(unixS) {
  const d = new Date(unixS * 1000);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

// UNIX seconds → local "HH:MM:SS".
export function formatHHMMSS(unixS) {
  const d = new Date(unixS * 1000);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

// Distance from live in seconds → "−MM:SS", or "−H:MM:SS" from one hour on (REQ-43).
export function formatDistance(seconds) {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const rest = `${pad2(m)}:${pad2(s % 60)}`;
  return h > 0 ? `${MINUS}${h}:${rest}` : `${MINUS}${rest}`;
}
