const $ = (selector) => document.querySelector(selector);

const shelfView = $("#shelf-view");
const settingsView = $("#settings-view");
const library = $("#library");
const shelf = $("#shelf");
const shelfEmpty = $("#shelf-empty");
const gameCount = $("#game-count");
const lede = $("#lede");
const search = $("#search");
const filterButtons = document.querySelectorAll(".filter");
const loginBox = $("#login-box");
const loginText = $("#login-text");
const loginButton = $("#start-login");
const qr = $("#qr");
const qrWrap = $("#qr-wrap");
const userCode = $("#user-code");
const detailOverlay = $("#detail-overlay");
const detailBody = $("#detail-body");
const dock = $("#dock");
const dockArt = $("#dock-art");
const dockTitle = $("#dock-title");
const dockStatus = $("#dock-status");
const dockMeter = $("#dock-meter");
const dockAction = $("#dock-action");
const form = $("#settings-form");
const settingsNote = $("#settings-note");
const catalogStatus = $("#catalog-status");
const catalogNote = $("#catalog-note");
const catalogReload = $("#catalog-reload");

const signedOutHint = "登录后才能看到网盘里的游戏。";
const missingKeyHint = "先到设置里填写 AppKey 和 SecretKey 并保存。";
const regionText = { UP: "美服", EP: "欧服", JP: "日服", HP: "港服", KP: "韩服", IP: "国际服" };
const storeLocaleText = { "zh-hans-hk": "港服商店（简体）", "zh-hant-hk": "港服商店（繁体）", "en-us": "美服商店" };
const metaStateNote = {
  pending: "正在从商店补全中文名和封面…",
  missing: "商店里没有找到这款游戏，可以在下面手动改名。",
  offline: "商店暂时连不上，稍后会自动重试。",
};

let games = [];
let filter = "all";
let query = "";
let loggedIn = false;
let loginTimer = 0;
let metaTimer = 0;
let metaRounds = 0;
let job = null;
let jobTimer = 0;
let dockVisible = false;
let detail = null;
const tiles = new Map();

async function api(path, body) {
  const response = await fetch(path, body === undefined ? undefined : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || "请求没有完成");
  return data;
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function displayName(game) {
  return (game.display_name || "").trim() || (game.name || "").trim() || (game.title_id || "").trim() || "未知目录";
}

function platformText(game) {
  if (game.platform) return game.platform.toUpperCase();
  const id = (game.title_id || "").toUpperCase();
  if (id.startsWith("PPSA")) return "PS5";
  if (id.startsWith("CUSA")) return "PS4";
  return "";
}

function formatBytes(bytes) {
  if (!bytes) return "";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + " MB";
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + " GB";
}

function hashHue(text) {
  let hash = 0;
  for (const ch of text) hash = (hash * 33 + ch.codePointAt(0)) >>> 0;
  return hash % 360;
}

// 封面：有商店图用商店图，没有就用 Title ID 生成一张。
function buildArt(game, className) {
  const art = el("span", "art " + (className || ""));
  const fallback = () => {
    const hue = hashHue(game.title_id || game.name || "?");
    art.style.background = `linear-gradient(160deg, hsl(${hue} 42% 34%), hsl(${(hue + 40) % 360} 46% 18%))`;
    const box = el("span", "fallback");
    const id = (game.title_id || "").trim();
    if (id) {
      box.append(el("span", "fallback-kind", id.slice(0, 4)), el("span", "fallback-code", id.slice(4)));
    } else {
      box.append(el("span", "fallback-code", (game.name || "?").trim().slice(0, 2)));
    }
    art.append(box);
  };
  const url = (game.cover_url || "").trim();
  if (url) {
    const img = document.createElement("img");
    img.alt = "";
    img.loading = "lazy";
    img.decoding = "async";
    img.src = url;
    img.addEventListener("error", () => {
      img.remove();
      fallback();
    }, { once: true });
    art.append(img);
  } else {
    fallback();
  }
  const fill = el("span", "fill");
  fill.append(el("span"));
  art.append(fill);
  return art;
}

function jobFraction() {
  if (!job || !job.files_total) return 0;
  return Math.min(1, job.files_done / job.files_total);
}

function jobFor(game) {
  return job && job.running && job.path === game.path;
}

function paintArt(art, game) {
  art.dataset.state = game.state;
  const installing = jobFor(game);
  art.style.setProperty("--p", installing ? jobFraction() : 0);
  const label = art.querySelector(".fill span");
  if (label) label.textContent = installing && job.files_total ? Math.round(jobFraction() * 100) + "%" : "…";
  art.querySelector(".badge")?.remove();
  if (game.state === "installed") art.append(el("span", "badge installed", "已安装"));
  if (game.state === "continue") art.append(el("span", "badge continue", "未装完"));
}

/* 游戏库 */

function matches(game) {
  if (filter === "installed" && game.state !== "installed") return false;
  if (filter === "todo" && game.state === "installed") return false;
  if (!query) return true;
  const haystack = [displayName(game), game.name, game.title_id, game.catalog_name].join("\n").toLowerCase();
  return haystack.includes(query);
}

function renderShelf() {
  tiles.clear();
  shelf.replaceChildren();
  const list = games.filter(matches);
  for (const game of list) shelf.append(buildTile(game));
  gameCount.textContent = games.length ? String(games.length) : "";
  shelfEmpty.hidden = list.length > 0;
  if (games.length === 0) shelfEmpty.textContent = "游戏库目录里还没有游戏文件夹。";
  else if (query) shelfEmpty.textContent = `没有找到“${search.value.trim()}”。`;
  else if (filter === "installed") shelfEmpty.textContent = "还没有装到主机上的游戏。";
  else shelfEmpty.textContent = "这里没有游戏了。";
}

function buildTile(game) {
  const tile = el("button", "tile");
  tile.type = "button";
  const art = buildArt(game);
  const sub = el("span", "tile-sub");
  const platform = platformText(game);
  if (platform) sub.append(el("span", "plat", platform));
  if (game.title_id) sub.append(el("span", "num", game.title_id));
  tile.append(art, el("span", "tile-name", displayName(game)), sub);
  tile.addEventListener("click", () => openDetail(game, tile));
  tiles.set(game.path, { tile, art });
  paintTile(game);
  return tile;
}

function paintTile(game) {
  const entry = tiles.get(game.path);
  if (!entry) return;
  entry.tile.dataset.state = game.state;
  paintArt(entry.art, game);
}

function updateLede() {
  lede.replaceChildren();
  if (games.length && games.every((game) => game.state === "need-settings")) {
    const go = el("button", "", "到设置里填写存放位置");
    go.type = "button";
    go.addEventListener("click", () => show("settings"));
    lede.append(go, "后才能安装。");
    return;
  }
  const installed = games.filter((game) => game.state === "installed").length;
  if (games.length) lede.textContent = installed ? `已装到主机 ${installed} 个。` : "点开游戏就能装到主机。";
}

async function loadGames() {
  try {
    const data = await api("/api/games");
    games = data.games || [];
    if (job && job.running) {
      const game = games.find((item) => item.path === job.path);
      if (game) game.state = "installing";
    }
    setLoggedIn(true);
    updateLede();
    renderShelf();
    scheduleMetaPoll(true);
  } catch (error) {
    if (loginFlowActive()) return;
    if (error.message === "百度未登录") {
      games = [];
      setLoggedIn(false);
      loginText.textContent = signedOutHint;
      return;
    }
    if (loggedIn) lede.textContent = loginErrorText(error.message);
    else loginText.textContent = loginErrorText(error.message);
  }
}

const sharedMetaFields = ["display_name", "name_source", "store_locale", "catalog_name", "cover_url", "meta_state"];

function shareMeta(source) {
  const sameGame = (game) => (source.title_id ? game.title_id === source.title_id : game.name === source.name);
  for (const game of games) {
    if (game === source || !sameGame(game)) continue;
    for (const field of sharedMetaFields) game[field] = source[field];
  }
}

function scheduleMetaPoll(reset) {
  if (reset) metaRounds = 0;
  window.clearTimeout(metaTimer);
  metaTimer = 0;
  const waiting = games.some((game) => game.meta_state === "pending") || (detail && detail.game.meta_state === "pending");
  if (!waiting || metaRounds >= 200) return;
  metaTimer = window.setTimeout(pollMeta, 3000);
}

async function pollMeta() {
  metaTimer = 0;
  metaRounds += 1;
  const pending = games.filter((game) => game.meta_state === "pending");
  if (detail && detail.game.meta_state === "pending" && !pending.includes(detail.game)) pending.push(detail.game);
  try {
    const data = await api("/api/meta", { names: pending.map((game) => game.name) });
    let changed = false;
    (data.items || []).forEach((item, index) => {
      const game = pending[index];
      if (!game || game.name !== item.name) return;
      if (item.meta_state !== game.meta_state || item.display_name !== game.display_name || item.cover_url !== game.cover_url) {
        changed = true;
        if (detail && detail.game === game) detail.reload();
      }
      Object.assign(game, item);
    });
    if (changed) renderShelf();
  } catch (error) {
    // 下一轮再试
  }
  scheduleMetaPoll(false);
}

/* 安装任务：一次只有一个，在整个页面里共享 */

function jobText(current) {
  if (current.running) {
    if (current.message === "正在取消…") return current.message;
    if (current.files_total > 0) return `已装 ${current.files_done} / ${current.files_total} 个文件`;
    return current.message || "正在读取压缩包…";
  }
  if (current.message) return current.message;
  if (current.status === "installed" || current.status === "already") return "安装完成";
  return current.status || "";
}

function settledState(current) {
  if (current.status === "installed" || current.status === "already") return "installed";
  if (current.status === "interrupted") return "continue";
  return "install";
}

// 列表刷新后详情里拿的可能是旧对象，所以按路径找出所有要更新的游戏。
function applyJob() {
  if (!job) return;
  const targets = games.filter((item) => item.path === job.path);
  const inDetail = detail && detail.game.path === job.path;
  if (inDetail && !targets.includes(detail.game)) targets.push(detail.game);
  for (const game of targets) {
    if (job.running) game.state = "installing";
    else if (job.status && game.state === "installing") game.state = settledState(job);
    paintTile(game);
  }
  if (inDetail) detail.paint();
  paintDock(targets[0] || null);
}

let dockArtPath = null;

function paintDock(game) {
  dock.hidden = !dockVisible || !job;
  if (dock.hidden) return;
  const state = job.running ? "installing" : job.status === "installed" || job.status === "already" ? "installed" : "error";
  dock.dataset.state = state;
  dockTitle.textContent = game ? displayName(game) : job.path.split("/").pop();
  dockStatus.textContent = jobText(job);
  if (dockArtPath !== job.path) {
    dockArtPath = job.path;
    dockArt.replaceChildren(buildArt(game || { name: dockTitle.textContent }));
  }
  dockMeter.style.setProperty("--p", state === "installed" ? 1 : jobFraction());
  dockAction.textContent = job.running ? "取消" : "关闭";
  dockAction.disabled = job.running && job.message === "正在取消…";
}

async function refreshJob() {
  try {
    const next = await api("/api/job");
    const finished = job && job.running && !next.running;
    job = next;
    applyJob();
    if (finished) {
      stopJobWatch();
      loadGames();
    }
  } catch (error) {
    // 下一秒再试
  }
}

function watchJob() {
  stopJobWatch();
  jobTimer = window.setInterval(refreshJob, 1000);
}

function stopJobWatch() {
  window.clearInterval(jobTimer);
  jobTimer = 0;
}

async function startInstall(game) {
  try {
    await api("/api/install", { path: game.path });
  } catch (error) {
    if (detail && detail.game === game) detail.fail(error.message);
    return;
  }
  job = { running: true, path: game.path, files_done: 0, files_total: 0, message: "正在读取压缩包…" };
  dockVisible = true;
  applyJob();
  watchJob();
}

async function cancelInstall() {
  try {
    await api("/api/cancel", {});
    if (job) job.message = "正在取消…";
    applyJob();
  } catch (error) {
    dockStatus.textContent = error.message;
  }
}

dockAction.addEventListener("click", () => {
  if (job && job.running) {
    cancelInstall();
    return;
  }
  dockVisible = false;
  paintDock(null);
});

$("#dock-open").addEventListener("click", () => {
  const game = job && games.find((item) => item.path === job.path);
  if (game) openDetail(game);
});

/* 详情抽屉 */

const actionText = {
  install: "安装到主机",
  continue: "继续安装",
  installed: "已安装到主机",
  installing: "正在安装…",
  unsupported: "这种压缩包还不能安装",
  empty: "目录里没有压缩包",
  "need-settings": "先到设置里填写存放位置",
};

function detailFacts(game) {
  const facts = [];
  const platform = platformText(game);
  if (game.title_id) facts.push(game.title_id);
  if (platform) facts.push(platform);
  if (regionText[game.region]) facts.push(regionText[game.region]);
  if (game.formats) facts.push(game.formats.toUpperCase());
  if (game.archive_bytes) facts.push(formatBytes(game.archive_bytes));
  return facts;
}

function nameSourceText(game) {
  if (game.name_source === "user") return "手动修改";
  if (game.name_source === "store") return storeLocaleText[game.store_locale] || "商店";
  if (game.name_source === "catalog") return "PlayStation-Titles 英文名";
  return "网盘目录名";
}

function openDetail(game, opener) {
  closeDetail();
  detailOverlay.hidden = false;
  document.body.classList.add("drawer-open");
  detailBody.replaceChildren();

  const hero = el("div", "hero");
  const backdrop = el("div", "hero-backdrop");
  const row = el("div", "hero-row");
  const artSlot = el("div");
  const heading = el("div");
  const title = el("h2");
  title.id = "detail-title";
  const facts = el("div", "facts num");
  heading.append(title, facts);
  row.append(artSlot, heading);
  hero.append(backdrop, row);

  const install = el("div", "drawer-section install");
  const action = el("button", "btn primary");
  action.type = "button";
  const progress = el("div", "progress");
  const progressRow = el("div", "progress-row");
  const progressText = el("span");
  progressText.setAttribute("role", "status");
  const cancel = el("button", "link", "取消安装");
  cancel.type = "button";
  progressRow.append(progressText, cancel);
  const meter = el("span", "meter");
  meter.append(el("span"));
  progress.append(meter, progressRow);
  const status = el("p", "status-text");
  status.setAttribute("role", "status");
  install.append(action, progress, status);

  const about = el("div", "drawer-section");
  const desc = el("p", "desc");
  const specs = el("dl", "specs");
  const metaNote = el("p", "meta-note");
  about.append(desc, specs, metaNote);

  const naming = el("details", "naming");
  const summary = el("summary", "", "名称与资料");
  const namingBody = el("div", "naming-body");
  const origin = el("dl", "specs");
  const rename = buildRenameForm(game, () => {
    detail?.paint();
    shareMeta(game);
    renderShelf();
    scheduleMetaPoll(true);
  });
  namingBody.append(origin, rename.form);
  naming.append(summary, namingBody);

  detailBody.append(hero, install, about, naming);

  let coverKey = null;
  const paint = () => {
    if (jobFor(game)) game.state = "installing";
    title.textContent = displayName(game);
    facts.replaceChildren(...detailFacts(game).map((text) => el("span", "", text)));
    if (coverKey !== (game.cover_url || "")) {
      coverKey = game.cover_url || "";
      artSlot.replaceChildren(buildArt(game, "hero-art"));
      backdrop.style.backgroundImage = coverKey ? `url("${coverKey}")` : "none";
    }
    paintArt(artSlot.firstChild, game);

    const running = jobFor(game);
    action.textContent = actionText[game.state] || "安装到主机";
    action.classList.toggle("done", game.state === "installed");
    action.disabled = running || ["installed", "unsupported", "empty"].includes(game.state) ||
      (game.state !== "need-settings" && !game.formats);
    progress.hidden = !running;
    if (running) {
      progressText.textContent = jobText(job);
      meter.style.setProperty("--p", jobFraction());
      cancel.disabled = job.message === "正在取消…";
    }
    const finished = job && !job.running && job.path === game.path && job.status;
    status.hidden = running || !finished;
    status.className = "status-text" + (finished && settledState(job) !== "installed" ? " warn" : "");
    if (finished) status.textContent = jobText(job);

    desc.textContent = game.description || "";
    desc.hidden = !game.description;
    const rows = [["发行商", game.publisher], ["类型", game.genres], ["发售日期", game.release_date]];
    specs.replaceChildren(...rows.filter(([, value]) => value).flatMap(([key, value]) => [el("dt", "", key), el("dd", "", value)]));
    specs.hidden = specs.childElementCount === 0;
    metaNote.textContent = game.catalog_matched === false && !metaStateNote[game.meta_state]
      ? "资料库里没有这款游戏，仍然可以按目录名安装。"
      : metaStateNote[game.meta_state] || "";
    metaNote.hidden = !metaNote.textContent;
    about.hidden = desc.hidden && specs.hidden && metaNote.hidden;

    const originRows = [["名称来源", nameSourceText(game)]];
    const english = (game.catalog_name || "").trim();
    if (english && english !== displayName(game)) originRows.push(["英文名", english]);
    if ((game.name || "").trim()) originRows.push(["网盘目录", game.name]);
    if (game.concept_id) originRows.push(["Concept ID", String(game.concept_id)]);
    origin.replaceChildren(...originRows.flatMap(([key, value]) => [el("dt", "", key), el("dd", "", value)]));
  };

  action.addEventListener("click", () => {
    if (game.state === "need-settings") {
      show("settings");
      return;
    }
    status.hidden = true;
    startInstall(game);
  });
  cancel.addEventListener("click", cancelInstall);

  const reload = () => api("/api/game", { path: game.path }).then((data) => {
    if (!detail || detail.game !== game) return;
    Object.assign(game, data);
    paint();
    rename.sync();
  }).catch((error) => {
    if (detail && detail.game === game) detail.fail(error.message);
  });

  detail = {
    game,
    opener,
    paint,
    reload,
    fail(message) {
      status.hidden = false;
      status.className = "status-text warn";
      status.textContent = message;
    },
  };
  paint();
  reload();
  $("#detail-close").focus();
}

function closeDetail() {
  if (detailOverlay.hidden) return;
  detailOverlay.hidden = true;
  document.body.classList.remove("drawer-open");
  const opener = detail && detail.opener;
  detail = null;
  if (opener && opener.isConnected) opener.focus();
}

function buildRenameForm(game, onSaved) {
  const formEl = el("form", "rename");
  const input = el("input");
  input.maxLength = 80;
  input.spellcheck = false;
  input.placeholder = "自己起个名字";
  input.setAttribute("aria-label", "自定义名称");
  const save = el("button", "btn primary small", "改名");
  save.type = "submit";
  const actions = el("div", "rename-actions");
  const reset = el("button", "btn quiet small", "恢复自动名称");
  reset.type = "button";
  const refetch = el("button", "btn quiet small", "重新获取商店资料");
  refetch.type = "button";
  actions.append(reset, refetch);
  const note = el("p", "note");
  note.setAttribute("role", "status");
  const line = el("div", "rename-line");
  line.append(input, save);
  formEl.append(line, actions, note);

  const sync = () => {
    input.value = game.name_source === "user" ? game.display_name : "";
    reset.hidden = game.name_source !== "user";
    refetch.hidden = !game.concept_id;
    actions.hidden = reset.hidden && refetch.hidden;
  };
  const send = async (path, body, done) => {
    note.textContent = "";
    note.classList.remove("error");
    try {
      Object.assign(game, await api(path, body));
      sync();
      onSaved();
      note.textContent = done;
    } catch (error) {
      note.classList.add("error");
      note.textContent = error.message;
    }
  };
  formEl.addEventListener("submit", (event) => {
    event.preventDefault();
    const value = input.value.trim();
    if (value) send("/api/name", { name: game.name, display_name: value }, "已改名。");
  });
  reset.addEventListener("click", () => send("/api/name", { name: game.name, display_name: "" }, "已恢复自动名称。"));
  refetch.addEventListener("click", () => send("/api/meta/refresh", { name: game.name }, "已排队，稍后自动更新。"));
  sync();
  return { form: formEl, sync };
}

$("#detail-backdrop").addEventListener("click", closeDetail);
$("#detail-close").addEventListener("click", closeDetail);
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && !detailOverlay.hidden) closeDetail();
});

/* 登录 */

function loginFlowActive() {
  return !qrWrap.hidden || loginTimer !== 0;
}

function loginErrorText(message) {
  if (message === "缺少 PANPLAY_BAIDU_APP_KEY" || message === "缺少 PANPLAY_BAIDU_SECRET_KEY") return missingKeyHint;
  return message;
}

function setLoggedIn(on) {
  loggedIn = on;
  loginBox.hidden = on && !loginFlowActive();
  library.hidden = !loginBox.hidden;
  loginButton.hidden = on;
}

function stopLoginPoll() {
  window.clearTimeout(loginTimer);
  loginTimer = 0;
}

function finishLogin() {
  stopLoginPoll();
  qrWrap.hidden = true;
  userCode.textContent = "";
  loginText.textContent = "已经登录，正在读取游戏…";
  loadGames();
}

async function pollLogin() {
  loginTimer = 0;
  try {
    const poll = await api("/api/login/poll", {});
    if (poll.done) {
      finishLogin();
      return;
    }
    if (poll.expired || poll.error) {
      stopLoginPoll();
      qrWrap.hidden = true;
      loginText.textContent = loginErrorText(poll.error || "二维码过期了，再显示一次。");
      return;
    }
    loginText.textContent = "在手机上确认后，这里会自动进入游戏库。";
    loginTimer = window.setTimeout(pollLogin, Math.max(5, poll.retry_after || 5) * 1000);
  } catch (error) {
    stopLoginPoll();
    loginText.textContent = loginErrorText(error.message);
  }
}

async function startLogin() {
  stopLoginPoll();
  loginButton.disabled = true;
  loginText.textContent = "正在向百度获取二维码…";
  let challenge;
  try {
    challenge = await api("/api/login/start", {});
  } finally {
    loginButton.disabled = false;
  }
  if (challenge.already) {
    qrWrap.hidden = true;
    loginText.textContent = "已经登录，正在读取游戏…";
    await loadGames();
    return;
  }
  if (challenge.qrcode_url) {
    qr.src = challenge.qrcode_url;
    qr.alt = "百度登录二维码";
    qrWrap.hidden = false;
  }
  userCode.textContent = challenge.user_code ? "用户码 " + challenge.user_code : "";
  loginText.textContent = "用百度 App、百度网盘或微信扫码。";
  loginButton.textContent = "换一张二维码";
  loginTimer = window.setTimeout(pollLogin, Math.max(5, challenge.interval || 5) * 1000);
}

loginButton.addEventListener("click", () => {
  startLogin().catch((error) => { loginText.textContent = loginErrorText(error.message); });
});

/* 设置 */

function formatTime(unixSeconds) {
  if (!unixSeconds || unixSeconds < 0) return "";
  const date = new Date(unixSeconds * 1000);
  return Number.isNaN(date.getTime()) ? "" : date.toLocaleString("zh-CN", { hour12: false });
}

function renderCatalogStatus(info) {
  const sync = info && info.sync ? info.sync : {};
  if (sync.phase === "updating") {
    catalogStatus.textContent = "正在后台同步…";
    return;
  }
  if (!info || !info.loaded) {
    catalogStatus.textContent = "还没有资料库，后台正在第一次同步。在这之前游戏按目录名显示。";
    return;
  }
  const lines = [`已收录 ${info.count.toLocaleString("zh-CN")} 款游戏。`];
  const updated = info.generated_at ? info.generated_at.replace("T", " ").replace("Z", " UTC")
    : formatTime(info.file_mtime) || formatTime(sync.last_success);
  if (updated) lines.push(`上次更新 ${updated}。`);
  if (info.stale) lines.push(`已超过 ${sync.stale_days || 7} 天，会自动重新同步。`);
  if (sync.last_error) lines.push(`最近一次同步失败：${sync.last_error}`);
  catalogStatus.textContent = lines.join("");
}

let catalogTimer = 0;

async function loadCatalogStatus() {
  const info = await api("/api/catalog");
  renderCatalogStatus(info);
  window.clearInterval(catalogTimer);
  catalogTimer = 0;
  if (info && info.sync && info.sync.phase === "updating") {
    catalogTimer = window.setInterval(() => { loadCatalogStatus().catch(() => {}); }, 5000);
  }
}

async function loadSettings() {
  const settings = await api("/api/settings");
  for (const name of ["app_key", "secret_key", "library_path", "dest", "prefix"]) {
    form.elements[name].value = settings[name] || "";
  }
  await loadCatalogStatus();
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const body = {};
  for (const name of ["app_key", "secret_key", "library_path", "dest", "prefix"]) body[name] = form.elements[name].value;
  settingsNote.classList.remove("error");
  try {
    await api("/api/settings", body);
    settingsNote.textContent = "已保存。";
    if (loggedIn) loadGames();
  } catch (error) {
    settingsNote.classList.add("error");
    settingsNote.textContent = error.message;
  }
});

catalogReload.addEventListener("click", async () => {
  catalogReload.disabled = true;
  catalogNote.classList.remove("error");
  catalogNote.textContent = "正在重新加载…";
  try {
    renderCatalogStatus(await api("/api/catalog/reload", {}));
    catalogNote.textContent = "已重新加载。";
    if (loggedIn) await loadGames();
  } catch (error) {
    catalogNote.classList.add("error");
    catalogNote.textContent = error.message;
  } finally {
    catalogReload.disabled = false;
  }
});

/* 导航与筛选 */

function show(view) {
  closeDetail();
  shelfView.hidden = view !== "shelf";
  settingsView.hidden = view !== "settings";
  for (const [selector, name] of [["#show-shelf", "shelf"], ["#show-settings", "settings"]]) {
    if (name === view) $(selector).setAttribute("aria-current", "page");
    else $(selector).removeAttribute("aria-current");
  }
  if (view === "settings") {
    settingsNote.textContent = "";
    loadSettings().catch((error) => {
      settingsNote.classList.add("error");
      settingsNote.textContent = error.message;
    });
  }
  window.scrollTo(0, 0);
}

$("#show-shelf").addEventListener("click", () => show("shelf"));
$("#show-settings").addEventListener("click", () => show("settings"));

search.addEventListener("input", () => {
  query = search.value.trim().toLowerCase();
  if (shelfView.hidden) show("shelf");
  renderShelf();
});

for (const button of filterButtons) {
  button.addEventListener("click", () => {
    filter = button.dataset.filter;
    for (const other of filterButtons) other.setAttribute("aria-pressed", String(other === button));
    renderShelf();
  });
}

/* 启动 */

async function boot() {
  loginButton.disabled = true;
  try {
    const session = await api("/api/session");
    if (!session.logged_in) {
      setLoggedIn(false);
      loginText.textContent = signedOutHint;
      return;
    }
    try {
      job = await api("/api/job");
    } catch (error) {
      job = null;
    }
    await loadGames();
    if (job && job.running) {
      dockVisible = true;
      applyJob();
      watchJob();
    }
  } catch (error) {
    setLoggedIn(false);
    loginText.textContent = loginErrorText(error.message);
  } finally {
    loginButton.disabled = false;
  }
}

boot();
