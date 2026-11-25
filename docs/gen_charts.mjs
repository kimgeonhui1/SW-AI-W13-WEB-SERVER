// [docs/gen_charts.mjs] README용 SVG 차트 생성기 — 라이트/다크 두 벌. 사용: node docs/gen_charts.mjs docs/img
import { writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const OUT = process.argv[2];
mkdirSync(OUT, { recursive: true });

const THEMES = {
  light: { surface: '#fcfcfb', border: 'rgba(11,11,11,0.10)', p: '#0b0b0b', s: '#52514e', m: '#898781',
           grid: '#e1e0d9', base: '#c3c2b7', s1: '#2a78d6', s2: '#eb6834', tile: '#f5f5f2' },
  dark:  { surface: '#1a1a19', border: 'rgba(255,255,255,0.10)', p: '#f0efec', s: '#c3c2b7', m: '#898781',
           grid: '#2c2c2a', base: '#383835', s1: '#3987e5', s2: '#d95926', tile: '#222221' },
};
const FONT = `system-ui, -apple-system, 'Segoe UI', 'Apple SD Gothic Neo', 'Malgun Gothic', 'Noto Sans KR', sans-serif`;
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const fmt = (n) => n.toLocaleString('en-US');
const compact = (n) => (n >= 1000 ? `${n / 1000}K` : `${n}`);

function frame(t, w, h, title, sub, body) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" font-family="${FONT}" role="img" aria-label="${esc(title)}">
<rect x="0.5" y="0.5" width="${w - 1}" height="${h - 1}" rx="12" fill="${t.surface}" stroke="${t.border}"/>
<text x="24" y="38" font-size="16" font-weight="600" fill="${t.p}">${esc(title)}</text>
${sub ? `<text x="24" y="60" font-size="12.5" fill="${t.s}">${esc(sub)}</text>` : ''}
${body}
</svg>\n`;
}

// 아래쪽이 기준선인 막대: 윗끝만 4px 둥글게, 기준선 쪽은 각지게
function colPath(x, y, w, yb) {
  const r = Math.min(4, (yb - y) / 2, w / 2);
  return `M${x},${yb} V${y + r} Q${x},${y} ${x + r},${y} H${x + w - r} Q${x + w},${y} ${x + w},${y + r} V${yb} Z`;
}
// 왼쪽이 기준선인 가로 막대: 오른끝만 둥글게
function barPath(x0, y, len, h) {
  const r = Math.min(4, len / 2, h / 2);
  return `M${x0},${y} H${x0 + len - r} Q${x0 + len},${y} ${x0 + len},${y + r} V${y + h - r} Q${x0 + len},${y + h} ${x0 + len - r},${y + h} H${x0} Z`;
}

// 글자 폭 추정: 한글은 글자 크기만큼, 영문·숫자·기호는 약 0.6배 (레이아웃 겹침 방지용, 넉넉하게)
function textWidth(s, size) {
  let w = 0;
  for (const ch of s) w += /[ㄱ-힣]/.test(ch) ? size : ch === ' ' ? size * 0.3 : size * 0.62;
  return w;
}

function legend(t, x, y, items) {
  let out = '', cx = x;
  for (const [color, label] of items) {
    out += `<rect x="${cx}" y="${y - 9}" width="10" height="10" rx="2" fill="${color}"/>`;
    out += `<text x="${cx + 16}" y="${y}" font-size="12.5" fill="${t.s}">${esc(label)}</text>`;
    cx += 16 + textWidth(label, 12.5) + 24;
  }
  return out;
}

// ── 1. 상단 배너 (stat tile 한 줄) ──
function hero(t) {
  const W = 880, H = 236;
  const tiles = [
    ['기능 테스트', '145 / 145', '실제 서버 · 일반 + ASan'],
    ['단위 테스트 검사', '732', '11개 파일 전부 통과'],
    ['메모리 오류 · 누수', '0', 'ASan · valgrind'],
    ['변이 테스트 검출', '61 / 63', '남은 2개는 등가 변이'],
    ['정적 파일 처리량', '92,750', '초당 요청 · 동시 10연결'],
  ];
  const gap = 12, tx0 = 24, tw = (W - 48 - gap * 4) / 5, ty = 96, th = 116;
  let body = '';
  tiles.forEach(([label, value, note], i) => {
    const x = tx0 + i * (tw + gap);
    body += `<rect x="${x}" y="${ty}" width="${tw}" height="${th}" rx="10" fill="${t.tile}" stroke="${t.border}"/>`;
    body += `<rect x="${x + 16}" y="${ty + 16}" width="20" height="3" rx="1.5" fill="${t.s1}"/>`;
    body += `<text x="${x + 16}" y="${ty + 40}" font-size="12.5" fill="${t.s}">${esc(label)}</text>`;
    body += `<text x="${x + 16}" y="${ty + 76}" font-size="28" font-weight="600" fill="${t.p}">${esc(value)}</text>`;
    body += `<text x="${x + 16}" y="${ty + 100}" font-size="11.5" fill="${t.m}">${esc(note)}</text>`;
  });
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" font-family="${FONT}" role="img" aria-label="SW-AI W13 Web Server 검증 요약">
<rect x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="14" fill="${t.surface}" stroke="${t.border}"/>
<text x="24" y="46" font-size="26" font-weight="700" fill="${t.p}">SW-AI W13 Web Server</text>
<text x="24" y="72" font-size="14" fill="${t.s}">POSIX 소켓만으로 만든 C 웹서버 · 정적 파일 · CGI · 스레드 풀 · Reverse / Forward Proxy · 11팀</text>
${body}
</svg>\n`;
}

// ── 묶음 세로 막대 (동시 연결 수별 처리량) ──
function groupedColumns(t, { title, sub, groups, series, max, ticks, labels }) {
  const W = 800, H = 400, x0 = 72, x1 = W - 32, yTop = 112, yb = H - 56;
  const ph = yb - yTop, y = (v) => yb - (v / max) * ph;
  let body = legend(t, 24, 88, series.map((s) => [t[s.color], s.name]));
  for (const tk of ticks) {
    const yy = y(tk);
    body += `<line x1="${x0}" x2="${x1}" y1="${yy}" y2="${yy}" stroke="${tk === 0 ? t.base : t.grid}" stroke-width="1"/>`;
    body += `<text x="${x0 - 10}" y="${yy + 4}" font-size="11.5" fill="${t.m}" text-anchor="end">${compact(tk)}</text>`;
  }
  const gw = (x1 - x0) / groups.length, bw = 24, gapB = 2;
  groups.forEach((g, gi) => {
    const cx = x0 + gw * gi + gw / 2;
    const total = series.length * bw + (series.length - 1) * gapB;
    series.forEach((s, si) => {
      const v = s.values[gi], bx = cx - total / 2 + si * (bw + gapB), by = y(v);
      body += `<path d="${colPath(bx, by, bw, yb)}" fill="${t[s.color]}"/>`;
      const lab = labels(si, gi, v);
      // 마지막 계열(오른쪽 막대)은 값이 작아 옆 막대와 겹치지 않게 막대 오른쪽에 붙인다
      const last = si === series.length - 1 && series.length > 1;
      if (lab) body += last
        ? `<text x="${bx + bw + 6}" y="${by - 2}" font-size="12" fill="${t.s}">${esc(lab)}</text>`
        : `<text x="${bx + bw / 2}" y="${by - 8}" font-size="12.5" font-weight="600" fill="${t.p}" text-anchor="middle">${esc(lab)}</text>`;
    });
    body += `<text x="${cx}" y="${yb + 24}" font-size="12.5" fill="${t.s}" text-anchor="middle">${esc(g)}</text>`;
  });
  return frame(t, W, H, title, sub, body);
}

// ── 가로 막대 (단일 계열, 값은 끝에) ──
function hbars(t, { title, sub, rows, max, labelW = 150, suffix = '' }) {
  const W = 800, rowH = 34, barH = 20, top = sub ? 92 : 72;
  const H = top + rows.length * rowH + 20;
  const bx0 = 24 + labelW, bxMax = W - 110;
  let body = `<line x1="${bx0}" x2="${bx0}" y1="${top - 8}" y2="${top + rows.length * rowH - 6}" stroke="${t.base}" stroke-width="1"/>`;
  rows.forEach(([name, v], i) => {
    const yy = top + i * rowH, len = Math.max(2, ((bxMax - bx0) * v) / max);
    body += `<text x="${bx0 - 12}" y="${yy + barH / 2 + 4.5}" font-size="13" fill="${t.s}" text-anchor="end">${esc(name)}</text>`;
    body += `<path d="${barPath(bx0, yy, len, barH)}" fill="${t.s1}"/>`;
    body += `<text x="${bx0 + len + 8}" y="${yy + barH / 2 + 4.5}" font-size="13" font-weight="600" fill="${t.p}">${esc(fmt(v) + suffix)}</text>`;
  });
  return frame(t, W, H, title, sub, body);
}

// ── 단계별 세로 막대 (병목 개선) ──
function steps(t) {
  const W = 800, H = 380, x0 = 72, x1 = W - 32, yTop = 96, yb = H - 64, max = 100000;
  const data = [['처음', 173, ''], ['TCP_NODELAY', 3058, '×17.7'], ['작은 파일 캐시', 92750, '×30.3']];
  const ph = yb - yTop, y = (v) => yb - (v / max) * ph;
  let body = '';
  for (const tk of [0, 25000, 50000, 75000, 100000]) {
    const yy = y(tk);
    body += `<line x1="${x0}" x2="${x1}" y1="${yy}" y2="${yy}" stroke="${tk === 0 ? t.base : t.grid}" stroke-width="1"/>`;
    body += `<text x="${x0 - 10}" y="${yy + 4}" font-size="11.5" fill="${t.m}" text-anchor="end">${compact(tk)}</text>`;
  }
  const gw = (x1 - x0) / data.length, bw = 24;
  data.forEach(([name, v, mult], i) => {
    const cx = x0 + gw * i + gw / 2, by = Math.min(y(v), yb - 2);
    body += `<path d="${colPath(cx - bw / 2, by, bw, yb)}" fill="${t.s1}"/>`;
    body += `<text x="${cx}" y="${by - 10}" font-size="13" font-weight="600" fill="${t.p}" text-anchor="middle">${fmt(v)}${mult ? `<tspan font-weight="400" fill="${t.s}"> (${mult})</tspan>` : ''}</text>`;
    body += `<text x="${cx}" y="${yb + 24}" font-size="12.5" fill="${t.s}" text-anchor="middle">${esc(name)}</text>`;
  });
  return frame(t, W, H, '병목을 고칠 때마다 — 정적 파일 처리량', '초당 요청 수 · 동시 10연결 · 괄호는 직전 단계 대비', body);
}

// ── 변이 테스트: 하나의 전체를 나눈 키 큰 막대 ──
function mutation(t) {
  const W = 800, H = 176, x = 24, y = 84, w = W - 48, h = 28, gap = 2;
  const caught = 61, eq = 2, total = caught + eq;
  const w1 = (w - gap) * caught / total, w2 = (w - gap) - w1;
  let body = '';
  body += `<rect x="${x}" y="${y}" width="${w1}" height="${h}" rx="4" fill="${t.s1}"/>`;
  body += `<rect x="${x + w1 + gap}" y="${y}" width="${w2}" height="${h}" rx="4" fill="${t.s2}"/>`;
  body += `<text x="${x + 12}" y="${y + 18.5}" font-size="13" font-weight="600" fill="#ffffff">61개 검출 · 97%</text>`;
  body += legend(t, 24, 146, [[t.s1, '테스트가 잡음 61'], [t.s2, '등가 변이 2 (지워도 동작이 같음)']]);
  return frame(t, W, H, '변이 테스트 63개 — 일부러 넣은 버그를 테스트가 잡았나', '서버 코어 24 · CGI 13 · Reverse 15 · Forward 11', body);
}

const charts = {
  hero,
  'perf-concurrency': (t) => groupedColumns(t, {
    title: '정적 파일 처리량 — 동시 연결 수별', sub: '초당 요청 수 · wrk 10초 · keep-alive · 워커 8개',
    groups: ['동시 10연결', '동시 100연결', '동시 500연결'],
    series: [{ name: '작은 파일 캐시 후', color: 's1', values: [92750, 41368, 37024] },
             { name: '캐시 전', color: 's2', values: [3120, 3211, 3166] }],
    max: 100000, ticks: [0, 25000, 50000, 75000, 100000],
    labels: (si, gi, v) => (si === 0 ? fmt(v) : gi === 0 ? '≈3.1K' : null),
  }),
  'perf-by-type': (t) => hbars(t, {
    title: '처리 종류별 처리량 — 동시 100연결', sub: '초당 요청 수 · CGI는 요청마다 fork + execve, 프록시는 백엔드 연결을 하나 더 맺음',
    rows: [['정적 파일', 41368], ['Reverse Proxy', 16800], ['CGI', 1250]], max: 45000, labelW: 120,
  }),
  'perf-steps': steps,
  'tests-by-area': (t) => hbars(t, {
    title: '기능 테스트 145개 — 영역별', sub: '실제 서버를 띄워 curl · nc로 확인 (tests/functional.sh)',
    rows: [['시작 · 인자', 42], ['응답 · 정적 · CGI', 35], ['프록시', 24], ['종료 · 자원', 18], ['동시성 · 큐', 16], ['요청 읽기', 10]],
    max: 45, labelW: 140, suffix: '개',
  }),
  'unit-by-file': (t) => hbars(t, {
    title: '단위 테스트 검사 732개 — 파일별', sub: 'tests/test_*.c · 일반 빌드와 ASan/UBSan 빌드 모두 통과',
    rows: [['rio', 156], ['proxy', 81], ['static_response', 80], ['http_request', 73], ['router', 66], ['net', 58],
           ['cgi', 53], ['forward_proxy', 53], ['server', 51], ['threadpool', 32], ['mime', 29]],
    max: 160, labelW: 130, suffix: '개',
  }),
  mutation,
};

for (const [name, fn] of Object.entries(charts)) {
  for (const mode of ['light', 'dark']) writeFileSync(join(OUT, `${name}-${mode}.svg`), fn(THEMES[mode]));
}
console.log('wrote', Object.keys(charts).length * 2, 'files to', OUT);
