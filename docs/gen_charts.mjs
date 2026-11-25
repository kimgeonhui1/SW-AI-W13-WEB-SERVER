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
    ['단위 테스트 검사문', '732', 'CHECK 문 · 전부 통과'],
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
    title: '단위 테스트 검사문 732개 — 파일별', sub: 'tests/test_*.c의 CHECK 문 개수 · 일반 빌드와 ASan/UBSan 빌드 모두 통과',
    rows: [['rio', 156], ['proxy', 81], ['static_response', 80], ['http_request', 73], ['router', 66], ['net', 58],
           ['cgi', 53], ['forward_proxy', 53], ['server', 51], ['threadpool', 32], ['mime', 29]],
    max: 160, labelW: 130, suffix: '개',
  }),
  mutation,
};

// ───────────── 구조도 (노드 · 화살표) ─────────────
// 역할별 왼쪽 띠 색: 메인 = 파랑, 워커 루프 = 회색, 프록시 = 보라, CGI·정적 = 청록, 오류·종료 = 빨강
const ROLE = {
  light: { main: '#2a78d6', worker: '#898781', proxy: '#6250d6', handler: '#1baf7a', err: '#d03b3b', panel: '#f3f3f0', node: '#ffffff', nodeBorder: 'rgba(11,11,11,0.14)' },
  dark:  { main: '#3987e5', worker: '#898781', proxy: '#9085e9', handler: '#199e70', err: '#d03b3b', panel: '#20201f', node: '#2f2f2c', nodeBorder: 'rgba(255,255,255,0.16)' },
};
const MODE_OF = (t) => (t === THEMES.light ? 'light' : 'dark');

function node(t, n, dim) {
  const r = ROLE[MODE_OF(t)];
  const op = dim ? ' opacity="0.32"' : '';
  return `<g${op}><rect x="${n.x}" y="${n.y}" width="${n.w}" height="${n.h}" rx="8" fill="${r.node}" stroke="${r.nodeBorder}"/>` +
    `<rect x="${n.x}" y="${n.y}" width="4" height="${n.h}" rx="2" fill="${r[n.role]}"/>` +
    `<text x="${n.x + 14}" y="${n.y + (n.sub ? 20 : n.h / 2 + 4.5)}" font-size="13" font-weight="600" fill="${t.p}">${esc(n.title)}</text>` +
    (n.sub ? `<text x="${n.x + 14}" y="${n.y + 37}" font-size="11.5" fill="${t.s}">${esc(n.sub)}</text>` : '') + '</g>';
}
function edge(t, e, dim) {
  const op = dim ? ' opacity="0.32"' : '';
  const d = 'M' + e.pts.map((p) => p.join(',')).join(' L');
  let out = `<g${op}><path d="${d}" fill="none" stroke="${t.m}" stroke-width="1.5" stroke-linejoin="round"${e.noArrow ? '' : ' marker-end="url(#ah)"'}/>`;
  if (e.label) {
    const [lx, ly] = e.at, anchor = e.anchor || 'middle', tw = textWidth(e.label, 11.5);
    const rx = anchor === 'middle' ? lx - tw / 2 - 4 : anchor === 'end' ? lx - tw - 4 : lx - 4;
    out += `<rect x="${rx}" y="${ly - 12}" width="${tw + 8}" height="17" rx="3" fill="${e.bg || t.surface}"/>`;
    out += `<text x="${lx}" y="${ly}" font-size="11.5" fill="${t.s}" text-anchor="${anchor}">${esc(e.label)}</text>`;
  }
  return out + '</g>';
}
const ARROW = (t) => `<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="${t.m}"/></marker></defs>`;

// 전체 흐름: 위 = 메인 스레드, 아래 = 워커 스레드 (handle_connection 한 함수 안)
const FLOW = {
  panels: [
    { x: 16, y: 60, w: 868, h: 164, label: '메인 스레드 · 1개 — 연결을 받아 큐에 넣기만 함' },
    { x: 16, y: 240, w: 868, h: 500, label: '워커 스레드 · N개 — handle_connection(connfd) 한 함수 안에서 반복' },
  ],
  nodes: [
    { id: 'main', x: 32, y: 94, w: 120, h: 46, role: 'main', title: 'main.c', sub: '인자 · 시그널' },
    { id: 'run', x: 172, y: 94, w: 120, h: 46, role: 'main', title: 'run_server()', sub: 'server.c' },
    { id: 'listen', x: 312, y: 94, w: 150, h: 46, role: 'main', title: '① open_listenfd()', sub: 'socket → bind → listen' },
    { id: 'pool', x: 482, y: 94, w: 160, h: 46, role: 'main', title: '② 워커 N개 생성', sub: '큐 앞에서 잠들어 대기' },
    { id: 'accept', x: 662, y: 94, w: 206, h: 46, role: 'main', title: '③ pselect + accept', sub: '연결을 받아 fd 하나' },
    { id: 'submit', x: 604, y: 170, w: 152, h: 42, role: 'main', title: '큐에 넣고 깨움', sub: 'threadpool_submit' },
    { id: 'busy', x: 772, y: 170, w: 96, h: 42, role: 'err', title: '즉시 503', sub: 'reject_busy' },
    { id: 'hc', x: 420, y: 276, w: 216, h: 46, role: 'worker', title: 'handle_connection(fd)', sub: '⑤ 버퍼 8KB · 수신 5초 · 송신 10초' },
    { id: 'read', x: 420, y: 356, w: 216, h: 46, role: 'worker', title: 'read_request()', sub: 'rio.c · ⑥ 리턴값 0 / 양수 / -1' },
    { id: 'rerr', x: 32, y: 356, w: 150, h: 46, role: 'err', title: 'reply_error()', sub: '오류 응답 · keep = 0' },
    { id: 'disp', x: 420, y: 446, w: 216, h: 46, role: 'worker', title: '⑦ dispatch()', sub: '직전에 keep 결정 · 4갈래 분기' },
    { id: 'fwd', x: 210, y: 522, w: 150, h: 50, role: 'proxy', title: 'Forward Proxy', sub: 'http://… 절대 URL · -f' },
    { id: 'rev', x: 372, y: 522, w: 150, h: 50, role: 'proxy', title: 'Reverse Proxy', sub: 'routes.conf 경로' },
    { id: 'cgi', x: 534, y: 522, w: 150, h: 50, role: 'handler', title: 'CGI', sub: '/cgi-bin/ · fork + execve' },
    { id: 'st', x: 696, y: 522, w: 150, h: 50, role: 'handler', title: '정적 파일', sub: '그 외 · 작은 파일 캐시' },
    { id: 'relay', x: 276, y: 598, w: 180, h: 42, role: 'proxy', title: 'proxy_relay()', sub: '응답 검사 · 502 / 504' },
    { id: 'resp', x: 592, y: 598, w: 196, h: 42, role: 'handler', title: 'send_headers / response', sub: 'Content-Length 붙여 응답' },
    { id: 'dec', x: 420, y: 676, w: 216, h: 46, role: 'worker', title: '⑧ 계속할지 판단', sub: 'keep · 리턴값 hr' },
    { id: 'close', x: 32, y: 676, w: 150, h: 46, role: 'err', title: 'lingering_close()', sub: '남은 입력 비우고 끊기' },
  ],
  edges: [
    { pts: [[152, 117], [172, 117]] }, { pts: [[292, 117], [312, 117]] }, { pts: [[462, 117], [482, 117]] }, { pts: [[642, 117], [662, 117]] },
    { pts: [[680, 140], [680, 170]], label: '④ 큐 여유', at: [686, 160], anchor: 'start', bg: 'panel' },
    { pts: [[820, 140], [820, 170]], label: '가득', at: [826, 160], anchor: 'start', bg: 'panel' },
    { pts: [[680, 212], [680, 256], [528, 256], [528, 276]], label: '워커가 큐에서 꺼냄', at: [690, 252], anchor: 'start', bg: 'panel' },
    { pts: [[528, 322], [528, 356]] },
    { pts: [[420, 379], [182, 379]], label: '파싱 오류 (리턴값 > 0)', at: [301, 374], bg: 'panel' },
    { pts: [[80, 402], [80, 676]] },
    { pts: [[450, 402], [450, 420], [150, 420], [150, 676]], label: '끊김 · 유휴 초과 (-1)', at: [300, 438], bg: 'panel' },
    { pts: [[528, 402], [528, 446]], label: '성공 (0)', at: [536, 430], anchor: 'start', bg: 'panel' },
    { pts: [[528, 492], [528, 506]], noArrow: true }, { pts: [[285, 506], [771, 506]], noArrow: true },
    { pts: [[285, 506], [285, 522]] }, { pts: [[447, 506], [447, 522]] }, { pts: [[609, 506], [609, 522]] }, { pts: [[771, 506], [771, 522]] },
    { id: 'pr1', pts: [[285, 572], [285, 586], [447, 586]], noArrow: true }, { id: 'pr2', pts: [[447, 572], [447, 586]], noArrow: true },
    { id: 'pr3', pts: [[366, 586], [366, 598]] },
    { id: 'rs1', pts: [[609, 572], [609, 586], [771, 586]], noArrow: true }, { id: 'rs2', pts: [[771, 572], [771, 586]], noArrow: true },
    { id: 'rs3', pts: [[690, 586], [690, 598]] },
    { pts: [[366, 640], [366, 656], [500, 656], [500, 676]] }, { pts: [[690, 640], [690, 656], [556, 656], [556, 676]] },
    { pts: [[420, 699], [182, 699]], label: 'keep-alive가 아니거나 오류', at: [301, 693], bg: 'panel' },
    { pts: [[636, 699], [866, 699], [866, 379], [636, 379]], label: 'keep-alive 유지 · 다음 요청', at: [760, 374], bg: 'panel' },
  ],
};
const HANDLER_IDS = new Set(['fwd', 'rev', 'cgi', 'st', 'relay', 'resp']);

function flow(t, { title, sub, clip, dimHandlers = false }) {
  const [cx, cy, cw, ch] = clip, head = 70;
  const r = ROLE[MODE_OF(t)];
  let g = '';
  for (const p of FLOW.panels) {
    g += `<rect x="${p.x}" y="${p.y}" width="${p.w}" height="${p.h}" rx="10" fill="${r.panel}" stroke="${t.border}"/>`;
    g += `<text x="${p.x + 14}" y="${p.y + 22}" font-size="12" font-weight="600" fill="${t.s}">${esc(p.label)}</text>`;
  }
  for (const e of FLOW.edges) {
    const dim = dimHandlers && (e.id || (e.pts[0][1] >= 506 && e.pts[0][1] < 676 && !e.label));
    g += edge(t, { ...e, bg: e.bg === 'panel' ? r.panel : e.bg }, dim);
  }
  for (const n of FLOW.nodes) g += node(t, n, dimHandlers && HANDLER_IDS.has(n.id));
  const W = cw, H = ch + head;
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" font-family="${FONT}" role="img" aria-label="${esc(title)}">
${ARROW(t)}
<rect x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="12" fill="${t.surface}" stroke="${t.border}"/>
<text x="24" y="36" font-size="16" font-weight="600" fill="${t.p}">${esc(title)}</text>
<text x="24" y="57" font-size="12.5" fill="${t.s}">${esc(sub)}</text>
<svg x="0" y="${head}" width="${cw}" height="${ch}" viewBox="${cx} ${cy} ${cw} ${ch}">${g}</svg>
</svg>\n`;
}

// 간단한 노드·화살표 그림 (CGI, 프록시)
function simple(t, { title, sub, W, H, nodes, edges }) {
  let g = '';
  for (const e of edges) g += edge(t, e, false);
  for (const n of nodes) g += node(t, n, false);
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" font-family="${FONT}" role="img" aria-label="${esc(title)}">
${ARROW(t)}
<rect x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="12" fill="${t.surface}" stroke="${t.border}"/>
<text x="24" y="36" font-size="16" font-weight="600" fill="${t.p}">${esc(title)}</text>
<text x="24" y="57" font-size="12.5" fill="${t.s}">${esc(sub)}</text>
${g}
</svg>\n`;
}

const CGI_FIG = {
  title: 'CGI 실행 — 출력을 파이프로 먼저 받아서 응답', sub: 'cgi.c · 자식은 execve 전까지 async-signal-safe 함수만 사용', W: 900, H: 320,
  nodes: [
    { x: 24, y: 104, w: 190, h: 52, role: 'worker', title: '워커', sub: 'fork 전에 환경변수 준비' },
    { x: 320, y: 104, w: 200, h: 52, role: 'handler', title: 'CGI 프로세스 (adder 등)', sub: '자기 프로세스 그룹' },
    { x: 660, y: 104, w: 216, h: 52, role: 'worker', title: '워커가 출력 수집', sub: 'poll로 입출력 동시 · 5초 · 1MB' },
    { x: 660, y: 240, w: 216, h: 52, role: 'handler', title: '헤더 검사 + Content-Length', sub: '→ HTTP 응답 · 연결 유지 가능' },
    { x: 320, y: 240, w: 200, h: 52, role: 'err', title: '504 / 502', sub: '그룹 SIGKILL · waitpid 회수' },
  ],
  edges: [
    { pts: [[214, 118], [320, 118]], label: 'fork + execve', at: [267, 112] },
    { pts: [[214, 142], [320, 142]], label: 'POST 본문 → stdin', at: [267, 166] },
    { pts: [[520, 130], [660, 130]], label: '출력 → stdout', at: [590, 124] },
    { pts: [[768, 156], [768, 240]], label: '정상', at: [776, 202], anchor: 'start' },
    { pts: [[708, 156], [708, 196], [420, 196], [420, 240]], label: '5초 초과 · 1MB 초과 · 형식 오류', at: [564, 190] },
  ],
};

const PROXY_FIG = {
  title: 'Reverse · Forward Proxy — 같은 proxy_relay로 중계', sub: '다른 점은 어디로 접속할지를 누가 정하는가 (운영자의 routes.conf / 클라이언트의 URL)', W: 900, H: 380,
  nodes: [
    { x: 24, y: 150, w: 150, h: 52, role: 'worker', title: '클라이언트 요청', sub: 'dispatch에서 분기' },
    { x: 204, y: 150, w: 236, h: 52, role: 'proxy', title: 'proxy_relay()', sub: 'hop-by-hop 제거 · Connection: close' },
    { x: 476, y: 90, w: 210, h: 52, role: 'proxy', title: 'Reverse (-c routes.conf)', sub: '최장 prefix · Round-Robin' },
    { x: 476, y: 210, w: 210, h: 52, role: 'proxy', title: 'Forward (-f)', sub: 'URL에서 host · port · Host 재작성' },
    { x: 716, y: 90, w: 160, h: 52, role: 'handler', title: '백엔드 1 / 2', sub: '꺼진 백엔드는 건너뜀' },
    { x: 716, y: 210, w: 160, h: 52, role: 'handler', title: '원 서버', sub: 'connect 5초 제한' },
    { x: 476, y: 306, w: 210, h: 52, role: 'proxy', title: '응답 검사', sub: '깨진 응답 502 · 5초 초과 504' },
    { x: 204, y: 306, w: 236, h: 52, role: 'worker', title: '클라이언트로 전달', sub: '받는 즉시 스트리밍' },
  ],
  edges: [
    { pts: [[174, 176], [204, 176]] },
    { pts: [[440, 166], [458, 166], [458, 116], [476, 116]] },
    { pts: [[440, 186], [458, 186], [458, 236], [476, 236]] },
    { pts: [[686, 116], [716, 116]] }, { pts: [[686, 236], [716, 236]] },
    { pts: [[876, 116], [890, 116], [890, 332], [686, 332]] },
    { pts: [[796, 262], [796, 332]], noArrow: true },
    { pts: [[476, 332], [440, 332]] },
  ],
};

charts['flow-full'] = (t) => flow(t, { title: '전체 흐름', sub: '서버 시작부터 연결을 닫을 때까지 · 번호는 아래 표의 ①~⑧', clip: [0, 50, 900, 700] });
charts['flow-main'] = (t) => flow(t, { title: '1-1. 서버 시작과 연결 받기', sub: '메인 스레드 부분 확대', clip: [0, 52, 900, 180] });
charts['flow-worker'] = (t) => flow(t, { title: '1-2. 연결 하나 처리하기', sub: '워커 스레드 부분 확대 · 4갈래 처리는 1-3, 1-4에서', clip: [0, 232, 900, 516], dimHandlers: true });
charts['flow-cgi'] = (t) => simple(t, CGI_FIG);
charts['flow-proxy'] = (t) => simple(t, PROXY_FIG);

for (const [name, fn] of Object.entries(charts)) {
  for (const mode of ['light', 'dark']) writeFileSync(join(OUT, `${name}-${mode}.svg`), fn(THEMES[mode]));
}
console.log('wrote', Object.keys(charts).length * 2, 'files to', OUT);
