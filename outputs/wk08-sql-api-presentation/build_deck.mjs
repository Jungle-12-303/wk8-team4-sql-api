const { Presentation, PresentationFile } = await import("@oai/artifact-tool");
const fs = await import("node:fs/promises");
const path = await import("node:path");
const { fileURLToPath } = await import("node:url");

const OUT_DIR = path.dirname(fileURLToPath(import.meta.url));
const TMP_DIR = path.join(OUT_DIR, "tmp", "slides");
const PPTX_PATH = path.join(OUT_DIR, "output.pptx");

const W = 1280;
const H = 720;
const C = {
  bg: "#F7F4EA",
  ink: "#132238",
  muted: "#546174",
  teal: "#008F8C",
  amber: "#F7B32B",
  coral: "#E84855",
  green: "#3A8F5A",
  blue: "#2E5AAC",
  panel: "#FFFFFF",
  line: "#D7D8CE",
  darkPanel: "#172033",
  softTeal: "#DCEFED",
  softAmber: "#FFF1C7",
  softCoral: "#FFE4E8",
  softGreen: "#E4F2E9",
};

const FONT = {
  title: "Apple SD Gothic Neo",
  body: "Apple SD Gothic Neo",
  mono: "Menlo",
};

function addShape(slide, geometry, left, top, width, height, fill = C.panel, line = C.line) {
  const shape = slide.shapes.add({
    geometry,
    position: { left, top, width, height },
    fill,
    line: { style: "solid", fill: line, width: line === "none" ? 0 : 1.2 },
  });
  return shape;
}

function addText(slide, text, left, top, width, height, opts = {}) {
  const shape = addShape(slide, "rect", left, top, width, height, opts.fill ?? "#FFFFFF00", opts.line ?? "none");
  shape.text = text;
  shape.text.typeface = opts.typeface ?? FONT.body;
  shape.text.fontSize = opts.fontSize ?? 24;
  shape.text.color = opts.color ?? C.ink;
  shape.text.bold = opts.bold ?? false;
  shape.text.alignment = opts.alignment ?? "left";
  shape.text.verticalAlignment = opts.verticalAlignment ?? "top";
  shape.text.insets = opts.insets ?? { left: 4, right: 4, top: 4, bottom: 4 };
  shape.text.autoFit = opts.autoFit ?? "shrinkText";
  return shape;
}

function addTitle(slide, title, subtitle, n) {
  addShape(slide, "rect", 0, 0, W, H, C.bg, "none");
  addShape(slide, "rect", 0, 0, W, 12, n % 3 === 1 ? C.teal : n % 3 === 2 ? C.coral : C.amber, "none");
  addText(slide, String(n).padStart(2, "0"), 54, 34, 64, 34, {
    fontSize: 18,
    color: C.muted,
    bold: true,
    alignment: "left",
  });
  addText(slide, title, 112, 30, 760, 54, {
    fontSize: 31,
    bold: true,
    typeface: FONT.title,
  });
  if (subtitle) {
    addText(slide, subtitle, 112, 82, 720, 34, {
      fontSize: 17,
      color: C.muted,
    });
  }
}

function addPill(slide, text, left, top, width, color, fill) {
  const pill = addShape(slide, "roundRect", left, top, width, 42, fill, color);
  pill.text = text;
  pill.text.typeface = FONT.body;
  pill.text.fontSize = 18;
  pill.text.bold = true;
  pill.text.color = color;
  pill.text.alignment = "center";
  pill.text.verticalAlignment = "middle";
  pill.text.insets = { left: 12, right: 12, top: 8, bottom: 8 };
  return pill;
}

function addCard(slide, title, body, left, top, width, height, color) {
  addShape(slide, "roundRect", left, top, width, height, C.panel, color);
  addShape(slide, "rect", left, top, 8, height, color, "none");
  addText(slide, title, left + 26, top + 22, width - 46, 34, {
    fontSize: 21,
    bold: true,
    color,
  });
  addText(slide, body, left + 26, top + 62, width - 46, height - 82, {
    fontSize: 18,
    color: C.ink,
  });
}

function addNode(slide, label, left, top, width, fill, stroke = C.teal) {
  const node = addShape(slide, "roundRect", left, top, width, 62, fill, stroke);
  node.text = label;
  node.text.typeface = FONT.body;
  node.text.fontSize = 17;
  node.text.bold = true;
  node.text.color = C.ink;
  node.text.alignment = "center";
  node.text.verticalAlignment = "middle";
  node.text.insets = { left: 8, right: 8, top: 8, bottom: 8 };
  node.text.autoFit = "shrinkText";
  return node;
}

function addArrow(slide, left, top, width, color = C.muted) {
  const arrow = addShape(slide, "rightArrow", left, top, width, 26, color, "none");
  return arrow;
}

function addBulletText(slide, items, left, top, width, height, opts = {}) {
  const text = items.map((item) => `- ${item}`).join("\n");
  return addText(slide, text, left, top, width, height, {
    fontSize: opts.fontSize ?? 21,
    color: opts.color ?? C.ink,
    fill: opts.fill,
    line: opts.line,
  });
}

function notes(slide, text) {
  slide.speakerNotes.setText(text.trim());
}

async function saveArtifact(fileLike, outputPath) {
  if (fileLike && typeof fileLike.save === "function") {
    await fileLike.save(outputPath);
    return;
  }

  if (fileLike instanceof Uint8Array) {
    await fs.writeFile(outputPath, fileLike);
    return;
  }

  if (fileLike && typeof fileLike.arrayBuffer === "function") {
    const buffer = Buffer.from(await fileLike.arrayBuffer());
    await fs.writeFile(outputPath, buffer);
    return;
  }

  if (fileLike && fileLike.buffer instanceof ArrayBuffer) {
    await fs.writeFile(outputPath, Buffer.from(fileLike.buffer));
    return;
  }

  throw new Error(`Unsupported artifact output for ${outputPath}`);
}

const deck = Presentation.create({ slideSize: { width: W, height: H } });
deck.theme.colorScheme = {
  name: "SQL API",
  themeColors: {
    bg1: C.bg,
    bg2: C.panel,
    tx1: C.ink,
    tx2: C.muted,
    accent1: C.teal,
    accent2: C.amber,
    accent3: C.coral,
    accent4: C.green,
    accent5: C.blue,
  },
};

{
  const slide = deck.slides.add();
  addShape(slide, "rect", 0, 0, W, H, C.bg, "none");
  addShape(slide, "rect", 0, 0, 28, H, C.teal, "none");
  addShape(slide, "rect", W - 260, 0, 260, H, C.darkPanel, "none");
  addText(slide, "Mini DBMS\nSQL API Server", 84, 78, 680, 170, {
    fontSize: 52,
    bold: true,
    typeface: FONT.title,
    color: C.ink,
  });
  addText(slide, "기존 C SQL 엔진 위에 HTTP API, worker thread pool, read/write lock을 얹은 과제 구현", 88, 258, 690, 76, {
    fontSize: 24,
    color: C.muted,
  });
  addPill(slide, "core 재사용", 88, 372, 170, C.teal, C.softTeal);
  addPill(slide, "외부 API", 282, 372, 150, C.coral, C.softCoral);
  addPill(slide, "병렬 처리", 456, 372, 150, C.green, C.softGreen);
  addCard(slide, "발표의 결론", "작은 DB 엔진을 새로 갈아엎지 않고, 검증 가능한 서버 경계로 감싸 과제 요구사항을 충족했습니다.", 84, 492, 720, 130, C.amber);
  addText(slide, "C\nHTTP\nThread\nB+Tree", 1036, 124, 210, 430, {
    fontSize: 38,
    bold: true,
    color: "#FFFFFF",
    alignment: "center",
    verticalAlignment: "middle",
  });
  notes(slide, `이번 과제는 미니 DBMS를 외부에서 사용할 수 있는 API 서버로 만드는 것이었습니다. 저희 구현은 기존에 있던 C 기반 SQL 엔진을 갈아엎지 않고, 그 위에 HTTP 서버 계층을 얹는 방식으로 접근했습니다. 그래서 핵심은 두 가지입니다. 하나는 src/core/에 있는 sql_execute(), Table, B+Tree를 그대로 살리는 것, 다른 하나는 src/server/에서 요청 큐, worker thread, read/write lock, JSON 응답 계약을 추가하는 것입니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "요구사항과 구현 매핑", "과제 문장의 핵심을 코드의 책임 경계로 연결", 2);
  const rows = [
    ["외부 클라이언트에서 DBMS 사용", "GET /health, GET /metrics, POST /query"],
    ["SQL 요청 병렬 처리", "bounded queue + worker thread pool"],
    ["기존 SQL/B+Tree 활용", "sql_execute(), Table, B+Tree"],
    ["C 언어 구현", "src/core/*.c, src/server/*.c"],
    ["품질 검증", "unit test, HTTP smoke, Postman collections"],
  ];
  const left = 92;
  const top = 150;
  const rowH = 82;
  addShape(slide, "roundRect", left, top - 18, 1096, rowH * rows.length + 34, C.panel, C.line);
  rows.forEach((row, i) => {
    const y = top + i * rowH;
    const color = [C.teal, C.coral, C.green, C.blue, C.amber][i];
    addShape(slide, "ellipse", left + 28, y + 18, 34, 34, color, "none");
    addText(slide, row[0], left + 84, y + 9, 390, 50, { fontSize: 21, bold: true });
    addText(slide, row[1], left + 510, y + 9, 550, 50, { fontSize: 21, color: C.muted });
    if (i < rows.length - 1) addShape(slide, "rect", left + 24, y + rowH - 3, 1048, 1, C.line, "none");
  });
  notes(slide, `요구사항별로 보면, 외부 클라이언트는 HTTP endpoint로 접근합니다. 실제 SQL 실행은 POST /query가 담당하고, 상태 확인은 /health, 운영 지표는 /metrics로 분리했습니다. 병렬성은 accept loop가 요청을 queue에 넣고, 여러 worker thread가 꺼내 처리하는 구조입니다. 내부 DB 엔진은 기존 SQL 파서와 테이블, B+Tree를 재사용했습니다. 품질 쪽은 단위 테스트와 실제 HTTP smoke 확인, 그리고 edge/burst Postman collection으로 보완했습니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "전체 아키텍처", "요청은 서버 경계를 지나 기존 DB 엔진으로 들어간다", 3);
  const nodes = [
    ["client", 64, 200, 118, C.softTeal],
    ["accept()", 220, 200, 126, C.softAmber],
    ["HTTPRequestQueue", 386, 200, 186, C.softCoral],
    ["worker thread", 616, 200, 164, C.softGreen],
    ["db_server_execute()", 824, 200, 210, "#E6ECFF"],
    ["JSON response", 1078, 200, 154, C.softAmber],
  ];
  nodes.forEach(([label, x, y, w, fill]) => addNode(slide, label, x, y, w, fill));
  [184, 350, 576, 786, 1038].forEach((x) => addArrow(slide, x, 218, 30, C.muted));
  addShape(slide, "roundRect", 210, 360, 860, 168, "#FFFFFF", C.teal);
  addText(slide, "src/server", 236, 382, 150, 34, { fontSize: 22, bold: true, color: C.teal });
  addText(slide, "HTTP parsing, routing, bounded queue, worker threads, rwlock, metrics, JSON", 236, 426, 790, 50, {
    fontSize: 22,
    color: C.ink,
  });
  addShape(slide, "roundRect", 320, 550, 640, 90, "#FFFFFF", C.amber);
  addText(slide, "src/core", 348, 568, 130, 30, { fontSize: 22, bold: true, color: C.amber });
  addText(slide, "sql_execute() -> Table -> B+Tree", 498, 568, 410, 32, {
    fontSize: 24,
    bold: true,
    color: C.ink,
  });
  notes(slide, `전체 흐름은 이렇게 볼 수 있습니다. 클라이언트가 HTTP 요청을 보내면 서버는 accept()로 연결을 받고, 이 client_socket을 bounded queue에 넣습니다. worker thread는 queue에서 socket을 꺼내 HTTP 요청을 읽고 파싱한 다음, /query라면 db_server_execute()로 넘깁니다. 이 함수가 공유 DB 경계입니다. 여기서 lock과 metrics를 처리하고, 그 안쪽에서 기존 sql_execute()가 실제 INSERT나 SELECT를 수행합니다. 결과는 다시 JSON 응답으로 직렬화되어 클라이언트에게 돌아갑니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "HTTP API 계약", "작은 API지만 성공과 실패를 분명히 구분", 4);
  addCard(slide, "GET /health", '{"ok":true,"status":"healthy"}', 78, 158, 344, 150, C.teal);
  addCard(slide, "GET /metrics", "request/select/insert/error counters\nqueue_full, lock_timeout", 468, 158, 344, 150, C.blue);
  addCard(slide, "POST /query", 'body: {"query":"SELECT ..."}\nSQL 실행 결과 반환', 858, 158, 344, 150, C.coral);
  addText(slide, "성공 응답", 104, 382, 180, 34, { fontSize: 25, bold: true, color: C.green });
  addBulletText(slide, ["insert: insertedId", "select: rowCount, rows", "지원 SQL 범위의 index 표시: usedIndex"], 104, 424, 470, 130, {
    fontSize: 22,
  });
  addText(slide, "실패 응답", 688, 382, 180, 34, { fontSize: 25, bold: true, color: C.coral });
  addBulletText(slide, ["syntax_error / query_error", "503 queue_full", "503 lock_timeout"], 688, 424, 430, 130, {
    fontSize: 22,
  });
  addShape(slide, "roundRect", 294, 586, 692, 62, C.darkPanel, "none");
  addText(slide, 'POST /query  {"query":"SELECT * FROM users WHERE id = 1;"}', 322, 602, 640, 30, {
    fontSize: 19,
    typeface: FONT.mono,
    color: "#FFFFFF",
  });
  notes(slide, `API는 일부러 작게 잡았습니다. /health는 서버가 살아 있는지 보는 endpoint이고, /metrics는 요청 수, SELECT/INSERT 수, 오류 수, queue full, lock timeout 같은 운영 지표를 보여줍니다. /query는 JSON body의 query 문자열을 읽어 SQL을 실행합니다. 성공 응답에는 insert의 경우 insertedId, select의 경우 rowCount, rows, 그리고 usedIndex가 들어갑니다. 오류도 문자열만 던지지 않고 syntax_error, query_error, queue_full, lock_timeout처럼 분류해서 HTTP status와 함께 내려줍니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "Thread Pool과 Backpressure", "요청을 무한히 쌓지 않고 처리 용량을 드러낸다", 5);
  addNode(slide, "main thread\naccept()", 92, 190, 170, C.softAmber, C.amber);
  addArrow(slide, 280, 208, 44, C.muted);
  addShape(slide, "roundRect", 344, 150, 330, 150, "#FFFFFF", C.coral);
  addText(slide, "HTTPRequestQueue", 374, 172, 270, 34, { fontSize: 24, bold: true, color: C.coral, alignment: "center" });
  for (let i = 0; i < 5; i++) {
    addShape(slide, "roundRect", 374 + i * 52, 230, 40, 42, [C.teal, C.amber, C.green, C.blue, C.coral][i], "none");
  }
  addArrow(slide, 694, 208, 44, C.muted);
  addNode(slide, "worker 1", 770, 140, 142, C.softGreen, C.green);
  addNode(slide, "worker 2", 770, 228, 142, C.softGreen, C.green);
  addNode(slide, "worker N", 770, 316, 142, C.softGreen, C.green);
  addCard(slide, "queue full", "http_request_queue_push() 실패\n-> 503 queue_full", 96, 440, 486, 136, C.coral);
  addCard(slide, "thread reuse", "요청마다 새 thread 생성이 아니라\n미리 만든 worker가 요청을 가져감", 650, 440, 486, 136, C.teal);
  notes(slide, `동시성에서 첫 번째 축은 thread pool입니다. 서버는 요청마다 새 thread를 만드는 방식이 아니라, 시작할 때 worker thread들을 만들어 두고 요청을 배정합니다. main thread는 네트워크 연결을 받아 queue에 넣고, worker들은 queue에서 socket을 꺼내 처리합니다. 즉 HTTP 요청 처리는 병렬화하되, 공유 DB 일관성은 다음 단계의 read/write lock으로 제어합니다. queue가 꽉 차면 http_request_queue_push()가 실패하고, 서버는 즉시 503 queue_full을 응답합니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "공유 DB 보호: Read/Write Lock", "core 엔진은 단순하게 두고 서버 경계에서 동기화", 6);
  addShape(slide, "roundRect", 86, 160, 300, 160, C.softTeal, C.teal);
  addText(slide, "SELECT", 112, 190, 250, 42, { fontSize: 34, bold: true, color: C.teal, alignment: "center" });
  addText(slide, "read lock\n여러 읽기 동시 허용", 112, 244, 250, 52, { fontSize: 21, alignment: "center" });
  addShape(slide, "roundRect", 450, 160, 300, 160, C.softCoral, C.coral);
  addText(slide, "INSERT", 476, 190, 250, 42, { fontSize: 34, bold: true, color: C.coral, alignment: "center" });
  addText(slide, "write lock\nTable/B+Tree 수정 보호", 476, 244, 250, 52, { fontSize: 21, alignment: "center" });
  addShape(slide, "roundRect", 814, 160, 300, 160, C.softAmber, C.amber);
  addText(slide, "TIMEOUT", 840, 190, 250, 42, { fontSize: 34, bold: true, color: C.amber, alignment: "center" });
  addText(slide, "대기 초과\n503 lock_timeout", 840, 244, 250, 52, { fontSize: 21, alignment: "center" });
  addCard(slide, "설계 선택", "src/core/sql.c, table.c, bptree.c는 lock을 모르도록 유지하고, src/server/db_server.c에서만 공유 DB 접근을 감싼다.", 170, 424, 940, 130, C.blue);
  notes(slide, `두 번째 축은 공유 DB 보호입니다. 여러 worker가 동시에 같은 in-memory table에 접근하므로 lock이 필요합니다. 여기서 중요한 설계 선택은 core 엔진에 lock을 흩뿌리지 않았다는 점입니다. src/core/sql.c, table.c, bptree.c는 단일 실행 엔진처럼 유지하고, src/server/db_server.c가 그 앞에서 read/write lock을 잡습니다. SELECT는 여러 개가 동시에 읽을 수 있도록 read lock을 쓰고, INSERT는 table과 B+Tree를 수정하므로 write lock을 씁니다. lock을 너무 오래 기다리면 503 lock_timeout으로 실패시킵니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "SQL 엔진과 B+Tree 재사용", "id 조건은 인덱스, name/age 조건은 선형 검색", 7);
  const codeBox = addShape(slide, "roundRect", 84, 150, 580, 220, C.darkPanel, "none");
  addText(slide, "INSERT INTO users VALUES ('Alice', 20);\nSELECT * FROM users WHERE id = 1;\nSELECT * FROM users WHERE age >= 20;", 120, 190, 510, 126, {
    fontSize: 24,
    typeface: FONT.mono,
    color: "#FFFFFF",
    fill: "#FFFFFF00",
    line: "none",
  });
  addCard(slide, "B+Tree primary index", "id 조건은 table_find_by_id_condition()에서 B+Tree leaf를 따라 탐색", 720, 150, 420, 138, C.teal);
  addCard(slide, "Linear scan", "name, age 조건은 단순 scan으로 처리해 구현 범위를 명확히 유지", 720, 320, 420, 138, C.coral);
  addCard(slide, "외부 관찰성", "usedIndex는 지원 SQL 범위에서 WHERE id 경로를 탔는지 보여주는 표시", 202, 474, 836, 110, C.amber);
  notes(slide, `DB 엔진 쪽은 단일 users(id, name, age) 테이블을 다룹니다. insert를 하면 id가 자동 증가하고 B+Tree primary index에도 들어갑니다. select는 SELECT *와 단순 WHERE 조건을 지원합니다. WHERE id 조건은 B+Tree를 사용하고, name과 age 조건은 linear scan으로 처리합니다. usedIndex는 범용 실행 계획 분석기가 아니라, 현재 지원하는 SQL 범위에서 WHERE id 경로를 탔는지 보여주는 표시입니다.`);
}

{
  const slide = deck.slides.add();
  addTitle(slide, "검증 결과와 마무리", "작동 여부, API 계약, 한계를 함께 말한다", 8);
  addCard(slide, "Unit", "make unit_test server\n./build/bin/unit_test\nAll unit tests passed.", 78, 150, 348, 176, C.green);
  addCard(slide, "HTTP smoke", "/health: healthy\ninsertedId=1\nusedIndex=true", 466, 150, 348, 176, C.teal);
  addCard(slide, "Metrics", "totalRequests=4\nquery=2, select=1, insert=1\nerrors=0", 854, 150, 348, 176, C.blue);
  addText(slide, "비범위", 118, 424, 120, 34, { fontSize: 25, bold: true, color: C.coral });
  addBulletText(slide, ["DDL / 다중 테이블", "UPDATE / DELETE", "영속화", "auth / TLS / 인터넷 배포"], 118, 464, 390, 142, { fontSize: 21 });
  addCard(slide, "마무리 한 문장", "DBMS 전체 기능 확장보다, 작은 SQL 엔진을 외부 API와 동시성 경계로 안전하게 감싸는 데 집중했습니다.", 590, 424, 548, 142, C.amber);
  notes(slide, `마지막으로 검증입니다. 현재 세션에서 make unit_test server는 최신 상태였고, ./build/bin/unit_test는 All unit tests passed.로 통과했습니다. 추가로 로컬 HTTP 서버를 띄워 /health, insert, id 기반 select, /metrics까지 확인했습니다. metrics에서는 총 4개 요청, query 2개, select 1개, insert 1개, error 0으로 나왔습니다. 이번 구현의 초점은 DBMS 전체 기능을 넓히는 것이 아니라, 작은 SQL 엔진을 외부 API와 동시성 경계로 안전하게 감싸는 것이었습니다. 그래서 DDL, 다중 테이블, update/delete, 영속화, 인증과 TLS는 명시적으로 비범위로 남겼습니다.`);
}

await fs.mkdir(TMP_DIR, { recursive: true });

for (let i = 0; i < deck.slides.count; i++) {
  const slide = deck.slides.getItem(i);
  const png = await deck.export({ slide, format: "png", scale: 1 });
  await saveArtifact(png, path.join(TMP_DIR, `preview-${String(i + 1).padStart(2, "0")}.png`));
}

const pptx = await PresentationFile.exportPptx(deck);
await saveArtifact(pptx, PPTX_PATH);
console.log(PPTX_PATH);
